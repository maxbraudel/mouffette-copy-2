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
    property int statusWidth: 120

    readonly property color statusForeground: statusKind === SegmentedStatusCard.Connected
                                               ? Theme.connectedText
                                               : statusKind === SegmentedStatusCard.Warning
                                                 ? Theme.warningText : Theme.errorText
    readonly property color statusBackground: statusKind === SegmentedStatusCard.Connected
                                               ? Theme.connectedBackground
                                               : statusKind === SegmentedStatusCard.Warning
                                                 ? Theme.warningBackground : Theme.errorBackground

    TextMetrics {
        id: auxiliaryMetrics
        text: root.auxiliaryText
        font.pixelSize: Theme.titleFontSize
        font.bold: true
    }

    implicitWidth: segments.implicitWidth
    implicitHeight: Theme.controlHeight
    height: Theme.controlHeight

    Row {
        id: segments
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: primarySegment
            height: root.height
            width: Math.max(20, primaryLabel.implicitWidth + Theme.segmentPadding * 2)
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
            topRightRadius: root.auxiliaryVisible ? 0 : Theme.controlRadius
            bottomRightRadius: root.auxiliaryVisible ? 0 : Theme.controlRadius

            Text {
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
            width: visible ? Math.max(40, auxiliaryMetrics.advanceWidth + Theme.segmentPadding * 2) : 0
            color: "transparent"
            topRightRadius: Theme.controlRadius
            bottomRightRadius: Theme.controlRadius

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
        anchors.left: parent.right
        anchors.leftMargin: 8
    }
}
