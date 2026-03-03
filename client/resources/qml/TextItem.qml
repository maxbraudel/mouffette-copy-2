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
    property string preEditText: ""
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
        root.textCommitRequested(root.mediaId, textEditor.text)
        root.editing = false
    }

    onPrimaryDoubleClicked: function(mediaId, additive) {
        if (!root.textEditable)
            return
        root.selectRequested(mediaId, additive)
        root.preEditText = root.textContent || ""
        root.editing = true
        // Explicitly reset the editor text every time we enter edit mode.
        // The declarative `text: root.preEditText` binding on textEditor is
        // broken by user input after the first keystroke; subsequent sessions
        // would therefore show stale content if we relied only on the binding.
        textEditor.text = root.preEditText
        textEditor.forceActiveFocus()
    }

    // Display text node — renders the fill color using a READ-ONLY TextEdit.
    // Uses TextEdit (not Text) so both display and edit mode use the same
    // QTextDocumentLayout engine, giving pixel-identical line heights.
    // enabled:false prevents mouse event interception.
    // z:1 keeps it on top of the stroke Shape (z:0 default).
    TextEdit {
        id: textDisplayNode
        visible: !root.editing
        z: 1
        anchors.fill: parent
        anchors.margins: 4
        readOnly: true
        activeFocusOnTab: false
        enabled: false
        topPadding: {
            var extra = Math.max(0, height - contentHeight)
            if (root.verticalAlignment === "top")    return 0
            if (root.verticalAlignment === "bottom") return extra
            return extra * 0.5
        }
        leftPadding:   0
        rightPadding:  0
        bottomPadding: 0
        text: root.textContent || ""
        color: root.textColor
        textFormat: TextEdit.PlainText
        wrapMode: root.fitToTextEnabled ? TextEdit.NoWrap : TextEdit.Wrap
        renderType: Text.QtRendering
        horizontalAlignment: root.horizontalAlignment === "left"
            ? Text.AlignLeft
            : (root.horizontalAlignment === "right" ? Text.AlignRight : Text.AlignHCenter)
        font.family: root.fontFamily
        font.pixelSize: Math.max(1, root.fontPixelSize)
        font.weight: root.fontWeight
        font.italic: root.fontItalic
        font.underline: root.fontUnderline
        font.capitalization: root.fontUppercase ? Font.AllUppercase : Font.MixedCase
        font.kerning: true
        font.preferShaping: true
        font.hintingPreference: Font.PreferNoHinting
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
        itemHeight:        Math.max(1, textDisplayNode.height)
        horizontalAlignment: root.horizontalAlignment
        verticalAlignment:   root.verticalAlignment
        fitToText:         root.fitToTextEnabled
    }

    // Border display path — bottom layer rendered with outlineColor.
    // Uses QtQuick.Shapes which tessellates the SVG path once into GPU geometry;
    // subsequent pan/zoom uses scene graph matrix transforms — no re-rasterization.
    Shape {
        id: textStrokeShape
        visible: !root.editing && root.outlinePixels > 0
        anchors.fill: textDisplayNode
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

    Item {
        id: editViewport
        visible: root.editing
        anchors.fill: parent
        anchors.margins: 4
        clip: true

        TextEdit {
            id: textEditor
            anchors.fill: parent
            // Installs the trailing-space alignment fix once; the persistent
            // post-layout hook it registers keeps every relayout correct.
            Component.onCompleted: TextEditHelper.applyIncludeTrailingSpaces(textEditor)
            topPadding: {
                var extra = Math.max(0, height - contentHeight)
                if (root.verticalAlignment === "top")    return 0
                if (root.verticalAlignment === "bottom") return extra
                return extra * 0.5
            }
            leftPadding:   0
            rightPadding:  0
            bottomPadding: 0
            text: root.preEditText
            color: root.textColor
            textFormat: TextEdit.PlainText
            wrapMode: root.fitToTextEnabled ? TextEdit.NoWrap : TextEdit.Wrap
            focus: root.editing
            cursorVisible: root.editing
            selectByMouse: false
            renderType: Text.QtRendering
            horizontalAlignment: root.horizontalAlignment === "left"
                ? Text.AlignLeft
                : (root.horizontalAlignment === "right" ? Text.AlignRight : Text.AlignHCenter)
            font.family: root.fontFamily
            font.pixelSize: Math.max(1, root.fontPixelSize)
            font.weight: root.fontWeight
            font.italic: root.fontItalic
            font.capitalization: root.fontUppercase ? Font.AllUppercase : Font.MixedCase
            font.kerning: true
            font.preferShaping: true
            font.hintingPreference: Font.PreferNoHinting
            font.underline: root.fontUnderline

            Keys.onPressed: function(event) {
                var isEnter = (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                var hasCommitModifier = (event.modifiers & Qt.ControlModifier) || (event.modifiers & Qt.MetaModifier)
                if (isEnter && hasCommitModifier) {
                    root.textCommitRequested(root.mediaId, textEditor.text)
                    root.editing = false
                    event.accepted = true
                }
            }

            onTextChanged: {
                if (root.editing) {
                    root.textLiveUpdateRequested(root.mediaId, textEditor.text)
                }
            }
        }

        // Explicit mouse handler for cursor placement and drag selection.
        // Required because TextEdit.selectByMouse is unreliable when the item lives
        // inside a scaled/transformed viewport (Qt6 coordinate mapping issue).
        MouseArea {
            id: textEditMouseArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            // Track the anchor position when a drag-select begins.
            property int pressCharPos: 0

            onPressed: function(mouse) {
                textEditor.forceActiveFocus()
                var mapped = mapToItem(textEditor, mouse.x, mouse.y)
                var pos = textEditor.positionAt(mapped.x, mapped.y)
                pressCharPos = pos
                textEditor.cursorPosition = pos
                mouse.accepted = true
            }

            onPositionChanged: function(mouse) {
                if (pressed) {
                    var mapped = mapToItem(textEditor, mouse.x, mouse.y)
                    var pos = textEditor.positionAt(mapped.x, mapped.y)
                    textEditor.select(pressCharPos, pos)
                }
            }
        }
    }

}
