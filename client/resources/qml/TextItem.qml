import QtQuick 2.15
import QtQuick.Shapes 1.0
import Mouffette.Canvas 1.0

BaseMediaItem {
    id: root
    property string textContent: ""
    property string horizontalAlignment: "center"
    property string verticalAlignment: "center"
    property bool fitToTextEnabled: false
    property string fontFamily: "Arial"
    property int fontPixelSize: 22
    property int fontWeight: 400
    property bool fontItalic: false
    property bool fontUnderline: false
    property bool fontUppercase: false
    property color textColor: "#FFFFFFFF"
    property real outlineWidthPercent: 0.0
    property color outlineColor: "#FF000000"
    property bool highlightEnabled: false
    property color highlightColor: "#00000000"
    property bool textEditable: false
    property bool editing: false
    signal textCommitRequested(string mediaId, string text)
    signal textLiveUpdateRequested(string mediaId, string text)

    clip: true
    pointerEnabled: !root.editing
    doubleClickEnabled: true

    // Outline thickness in scene-space pixels derived from font size.
    // Fed into TextGlyphPath.outlinePixels which drives the QPainterPathStroker width.
    readonly property real outlinePixels: root.outlineWidthPercent > 0
        ? Math.max(1, Math.round(root.outlineWidthPercent * Math.max(1, root.fontPixelSize) / 100.0))
        : 0

    function commitAndStopEditing() {
        if (!root.editing) return
        root.textCommitRequested(root.mediaId, textDisplayNode.text)
        root.editing = false
        // Clear any selection left over from edit mode so it doesn't bleed
        // into display mode under the stroke shape.
        textDisplayNode.select(0, 0)
    }

    onPrimaryDoubleClicked: function(mediaId, additive) {
        if (!root.textEditable)
            return
        root.selectRequested(mediaId, additive)
        root.editing = true
        // The Binding on textDisplayNode.text is inactive while root.editing
        // is true, so this imperative assignment is the source-of-truth for
        // the initial edit content.
        textDisplayNode.text = root.textContent || ""
        textDisplayNode.forceActiveFocus()
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
    //               Text is set imperatively in onPrimaryDoubleClicked and
    //               the Binding is inactive (when:!root.editing is false).
    TextEdit {
        id: textDisplayNode
        z: 1
        anchors.fill: parent
        anchors.margins: 4
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
                root.textCommitRequested(root.mediaId, textDisplayNode.text)
                root.editing = false
                textDisplayNode.select(0, 0)
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
        visible: root.highlightEnabled && (root.textContent || "").length > 0 && !root.editing
        color: root.highlightColor
        radius: 2

        readonly property real contentMargin: 4
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

    // Glyph path engine — computes fill + stroke SVG paths once per content/style
    // change using QTextLayout + QRawFont + QPainterPathStroker (C++ retained).
    // Camera pan/zoom never triggers recompute: Shape caches GPU geometry and
    // moves it via transforms only.
    TextGlyphPath {
        id: glyphPath
        textContent:        root.textContent || ""
        fontFamily:         root.fontFamily
        fontPixelSize:      Math.max(1, root.fontPixelSize)
        fontWeight:         root.fontWeight
        fontItalic:         root.fontItalic
        fontUppercase:      root.fontUppercase
        outlinePixels:     root.outlinePixels
        itemWidth:         Math.max(1, textDisplayNode.width)
        horizontalAlignment: root.horizontalAlignment
        verticalAlignment:   root.verticalAlignment
        fitToText:         root.fitToTextEnabled
    }

    // Border display path — bottom layer rendered with outlineColor.
    // Uses QtQuick.Shapes which tessellates the SVG path once into GPU geometry;
    // subsequent pan/zoom uses scene graph matrix transforms — no re-rasterization.
    //
    // TextGlyphPath always produces top-aligned paths (vertOffset=0 baked in).
    // Vertical centering is applied here via the same topPadding that textDisplayNode
    // uses, so both the fill text and the stroke shape shift in the same QML binding
    // evaluation — eliminating the one-tick timer lag that caused border drift during
    // any resize operation.
    Shape {
        id: textStrokeShape
        visible: !root.editing && root.outlinePixels > 0
        anchors.fill: textDisplayNode
        // Shift the Shape's coordinate origin down by textDisplayNode.topPadding so
        // that y=0 in path space maps to the top of the first rendered glyph line,
        // exactly matching where textDisplayNode places its content.
        transform: Translate { y: textDisplayNode.topPadding }
        // Disable anti-aliasing at the Shape level; Qt Quick's curve renderer
        // handles sub-pixel quality internally.
        layer.enabled: false
        ShapePath {
            fillColor:   root.outlineColor
            strokeColor: "transparent"
            strokeWidth: 0
            fillRule:    ShapePath.WindingFill
            PathSvg { path: glyphPath.strokePath }
        }
    }


}
