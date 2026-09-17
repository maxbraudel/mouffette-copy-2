import QtQuick
import Mouffette.App

Item {
    id: root

    enum StatusKind {
        Connected,
        Warning,
        Error
    }

    property string primaryText: ""
    property string statusText: "DISCONNECTED"
    property int statusKind: SegmentedStatusCard.Error
    property string auxiliaryText: ""
    property bool auxiliaryVisible: auxiliaryText.length > 0
    property bool busy: false
    readonly property real primaryWidth: Math.max(20, primaryLabel.implicitWidth + Theme.segmentPadding * 2)
    readonly property real statusWidth: statusMetrics.maximumWidth + Theme.segmentPadding * 2
    readonly property real auxiliaryWidth: auxiliaryVisible
        ? Math.max(40, auxiliaryMetrics.maximumWidth + Theme.segmentPadding * 2) : 0
    readonly property real busyWidth: busy ? Theme.controlHeight + Theme.segmentPadding : 0
    readonly property bool availableStatus: statusText.trim().toUpperCase() === "AVAILABLE"

    readonly property color statusForeground: availableStatus
                                               ? Theme.availableText
                                               : statusKind === SegmentedStatusCard.Connected
                                               ? Theme.connectedText
                                               : statusKind === SegmentedStatusCard.Warning
                                                 ? Theme.warningText : Theme.errorText
    readonly property color statusBackground: availableStatus
                                               ? Theme.buttonBackground
                                               : statusKind === SegmentedStatusCard.Connected
                                               ? Theme.connectedBackground
                                               : statusKind === SegmentedStatusCard.Warning
                                                 ? Theme.warningBackground : Theme.errorBackground

    ConnectionStatusMetrics {
        id: statusMetrics
        text: root.statusText
        uppercase: true
        font: statusLabel.font
    }

    StateTextMetrics {
        id: auxiliaryMetrics
        text: root.auxiliaryText
        textVariants: Array.from({length: 101}, function(_, value) { return value + "%" })
        font: auxiliaryLabel.font
    }

    implicitWidth: primaryWidth + 1 + statusWidth
                   + (auxiliaryVisible ? 1 + auxiliaryWidth : 0) + busyWidth
    implicitHeight: Theme.controlHeight
    height: Theme.controlHeight

    Row {
        id: segments
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: primarySegment
            height: root.height
            width: Math.max(0, root.width - 1 - root.statusWidth
                           - (root.auxiliaryVisible ? 1 + root.auxiliaryWidth : 0)
                           - root.busyWidth)
            color: "transparent"
            topLeftRadius: Theme.controlRadius
            bottomLeftRadius: Theme.controlRadius

            Text {
                id: primaryLabel
                anchors.fill: parent
                anchors.leftMargin: Theme.segmentPadding
                anchors.rightMargin: Theme.segmentPadding
                text: root.primaryText
                color: Theme.text
                font.pixelSize: Theme.titleFontSize
                font.bold: true
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
        }

        Rectangle {
            width: 1
            height: root.height
            color: Theme.border
        }

        Rectangle {
            id: statusSegment
            objectName: "statusSegment"
            height: root.height
            width: root.statusWidth
            color: root.statusBackground
            topRightRadius: root.auxiliaryVisible || root.busy ? 0 : Theme.controlRadius
            bottomRightRadius: root.auxiliaryVisible || root.busy ? 0 : Theme.controlRadius

            Text {
                id: statusLabel
                anchors.fill: parent
                anchors.leftMargin: Theme.segmentPadding
                anchors.rightMargin: Theme.segmentPadding
                text: root.statusText
                color: root.statusForeground
                font.pixelSize: Theme.controlFontSize
                font.bold: true
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
        }

        Rectangle {
            visible: root.auxiliaryVisible
            width: visible ? 1 : 0
            height: root.height
            color: Theme.border
        }

        Rectangle {
            id: auxiliarySegment
            visible: root.auxiliaryVisible
            height: root.height
            width: root.auxiliaryWidth
            color: "transparent"
            topRightRadius: root.busy ? 0 : Theme.controlRadius
            bottomRightRadius: root.busy ? 0 : Theme.controlRadius

            Text {
                id: auxiliaryLabel
                anchors.fill: parent
                anchors.leftMargin: Theme.segmentPadding
                anchors.rightMargin: Theme.segmentPadding
                text: root.auxiliaryText
                color: Theme.text
                font.pixelSize: Theme.titleFontSize
                font.bold: true
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    // Painted last. Segment fills can never erase or shorten this border.
    Rectangle {
        objectName: "cardBorder"
        anchors.fill: parent
        z: 100
        color: "transparent"
        radius: Theme.controlRadius
        border.width: 1
        border.color: Theme.border
    }

    AppSpinner {
        visible: root.busy
        running: visible
        width: root.height
        height: root.height
        anchors.right: parent.right
        anchors.rightMargin: Theme.segmentPadding / 2
    }
}
