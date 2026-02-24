import QtQuick 2.15

// Bottom overlay panel for a selected text media item.
// Row 1: fit-to-text toggle | [H-align group: left, center, right] | [V-align group: top, center, bottom]
// Mirrors the legacy TextMediaItem alignment panel layout and segmented button style.
Item {
    id: root

    // ── inputs ──────────────────────────────────────────────────────────────
    property string mediaId: ""
    property bool   fitToTextEnabled:      true
    property string horizontalAlignment:   "center"   // "left" | "center" | "right"
    property string verticalAlignment:     "center"   // "top"  | "center" | "bottom"

    // ── signals ─────────────────────────────────────────────────────────────
    signal fitToTextToggleRequested(string mediaId)
    signal horizontalAlignRequested(string mediaId, string alignment)
    signal verticalAlignRequested(string mediaId, string alignment)
    signal overlayHoveredChanged(bool hovered)

    // ── internal layout constants ────────────────────────────────────────────
    readonly property real btnSize:      36
    readonly property real groupGap:     10
    readonly property real itemSpacing:  0    // segmented buttons touch each other
    readonly property real betweenGap:   4    // gap between standalone btn and segment groups

    // panelWidth/panelHeight so CanvasRoot can read them for centering
    readonly property real panelWidth:  implicitWidth
    readonly property real panelHeight: implicitHeight

    implicitWidth:  btnRow.childrenRect.width
    implicitHeight: btnSize

    // ── button row ───────────────────────────────────────────────────────────
    Row {
        id: btnRow
        x: 0
        y: 0

        // ---- fit-to-text (solo toggle) ----------------------------------------
        OverlayButton {
            implicitWidth:  root.btnSize
            implicitHeight: root.btnSize
            iconSource: "qrc:/icons/icons/text/fit-to-text.svg"
            isToggle:   true
            toggled:    root.fitToTextEnabled
            onClicked:  root.fitToTextToggleRequested(root.mediaId)
        }

        // gap between fit-to-text and H-align group
        Item { width: root.groupGap; height: root.btnSize }

        // ---- horizontal alignment (Leading / Middle / Trailing) ----------------
        OverlayButton {
            implicitWidth:  root.btnSize
            implicitHeight: root.btnSize
            iconSource:     "qrc:/icons/icons/text/horizontal-align-left.svg"
            isToggle:       true
            toggled:        root.horizontalAlignment === "left"
            segmentRole:    "leading"
            onClicked:      root.horizontalAlignRequested(root.mediaId, "left")
        }
        OverlayButton {
            implicitWidth:  root.btnSize
            implicitHeight: root.btnSize
            iconSource:     "qrc:/icons/icons/text/horizontal-align-center.svg"
            isToggle:       true
            toggled:        root.horizontalAlignment === "center"
            segmentRole:    "middle"
            onClicked:      root.horizontalAlignRequested(root.mediaId, "center")
        }
        OverlayButton {
            implicitWidth:  root.btnSize
            implicitHeight: root.btnSize
            iconSource:     "qrc:/icons/icons/text/horizontal-align-right.svg"
            isToggle:       true
            toggled:        root.horizontalAlignment === "right"
            segmentRole:    "trailing"
            onClicked:      root.horizontalAlignRequested(root.mediaId, "right")
        }

        // gap between H-align group and V-align group
        Item { width: root.groupGap; height: root.btnSize }

        // ---- vertical alignment (Leading / Middle / Trailing) ------------------
        OverlayButton {
            implicitWidth:  root.btnSize
            implicitHeight: root.btnSize
            iconSource:     "qrc:/icons/icons/text/vertical-align-top.svg"
            isToggle:       true
            toggled:        root.verticalAlignment === "top"
            segmentRole:    "leading"
            onClicked:      root.verticalAlignRequested(root.mediaId, "top")
        }
        OverlayButton {
            implicitWidth:  root.btnSize
            implicitHeight: root.btnSize
            iconSource:     "qrc:/icons/icons/text/vertical-align-center.svg"
            isToggle:       true
            toggled:        root.verticalAlignment === "center"
            segmentRole:    "middle"
            onClicked:      root.verticalAlignRequested(root.mediaId, "center")
        }
        OverlayButton {
            implicitWidth:  root.btnSize
            implicitHeight: root.btnSize
            iconSource:     "qrc:/icons/icons/text/vertical-align-bottom.svg"
            isToggle:       true
            toggled:        root.verticalAlignment === "bottom"
            segmentRole:    "trailing"
            onClicked:      root.verticalAlignRequested(root.mediaId, "bottom")
        }
    }
}
