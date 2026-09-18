import QtQuick
import QtQuick.Controls
import Mouffette.App
import "../components"

AppPanel {
    id: root

    property var detailProvider: null
    property var model: null
    property bool sceneMode: false
    property string emptyText: ""
    signal activated(string identifier)

    ListView {
        id: list
        objectName: root.sceneMode ? "sceneActivitiesList" : "clientsList"
        anchors.fill: parent
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.model
        currentIndex: -1

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        delegate: ItemDelegate {
            id: row
            required property int index
            required property string identifier
            required property string primaryText
            required property string secondaryText
            required property string badgeText
            required property int badgeKind
            required property bool selectable
            required property bool hasProject

            readonly property string rowIdentifier: identifier
            readonly property string primaryValue: primaryText
            readonly property string secondaryValue: secondaryText
            readonly property string badgeValue: badgeText
            readonly property int badgeKindValue: badgeKind
            readonly property bool canActivate: selectable
            readonly property bool ongoingProject: hasProject && !root.sceneMode
            readonly property bool availableBadge: badgeValue.trim().toUpperCase() === "AVAILABLE"
            readonly property bool twoLineLayout: root.sceneMode && secondaryValue.length > 0
            readonly property real mainLineTopMargin: twoLineLayout
                                                       ? 7
                                                       : Math.round((height - 22) / 2)

            width: list.width
            height: twoLineLayout ? 66 : 48
            enabled: canActivate
            hoverEnabled: true
            property string connectionDetail: ""
            onHoveredChanged: if (hovered && root.detailProvider) connectionDetail = root.detailProvider(rowIdentifier)
            ToolTip.visible: hovered && connectionDetail.length > 0
            ToolTip.text: connectionDetail
            ToolTip.delay: 400
            Timer {
                interval: 500
                running: row.hovered && root.detailProvider !== null
                repeat: true
                onTriggered: row.connectionDetail = root.detailProvider(row.rowIdentifier)
            }
            padding: 0

            background: Rectangle {
                color: row.hovered && row.enabled ? Theme.brandBlueLight : "transparent"
            }

            contentItem: Item {
                anchors.fill: parent

                Text {
                    id: primary
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: trailingContent.left
                    anchors.rightMargin: 10
                    anchors.top: parent.top
                    anchors.topMargin: row.mainLineTopMargin
                    height: 22
                    text: row.primaryValue
                    color: Theme.text
                    font.weight: Font.DemiBold
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                Row {
                    id: trailingContent
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.top: parent.top
                    anchors.topMargin: row.mainLineTopMargin
                    height: 22
                    spacing: 8

                    Row {
                        id: clientDeadline
                        visible: !root.sceneMode && row.secondaryValue.length > 0
                        height: 22
                        spacing: 0

                        // StyledText does not apply <font face>. Give each time
                        // value its own Text item so Qt uses its actual font.
                        Repeater {
                            model: row.secondaryValue.split(/(\b\d+:\d{2}\b)/)

                            delegate: Text {
                                required property int index
                                required property string modelData
                                height: clientDeadline.height
                                text: modelData
                                textFormat: Text.PlainText
                                color: Theme.mutedText
                                font.family: index % 2 === 1
                                    ? Theme.monospaceFontFamily : Qt.application.font.family
                                font.pixelSize: 12
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }

                    Rectangle {
                        id: projectBadge
                        visible: row.ongoingProject
                        width: projectBadgeLabel.implicitWidth + 18
                        height: 22
                        radius: height / 2
                        color: Theme.brandBlueLight

                        Text {
                            id: projectBadgeLabel
                            anchors.centerIn: parent
                            text: "Ongoing project"
                            color: Theme.brandBlue
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                    }

                    Rectangle {
                        id: badge
                        width: Math.max(54, badgeMetrics.maximumWidth + 18)
                        height: 22
                        radius: height / 2
                        color: row.availableBadge ? Theme.buttonBackground
                               : row.badgeKindValue === 0 ? Theme.connectedBackground
                               : row.badgeKindValue === 1 ? Theme.warningBackground
                                                         : Theme.errorBackground

                        ConnectionStatusMetrics {
                            id: badgeMetrics
                            text: row.badgeValue
                            font: badgeLabel.font
                            sceneStatus: root.sceneMode
                        }

                        Text {
                            id: badgeLabel
                            anchors.centerIn: parent
                            text: row.badgeValue
                            color: row.availableBadge ? Theme.availableText
                                   : row.badgeKindValue === 0 ? Theme.connectedText
                                   : row.badgeKindValue === 1 ? Theme.warningText
                                                             : Theme.errorText
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                    }
                }

                Text {
                    visible: root.sceneMode && text.length > 0
                    anchors.left: primary.left
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.top: trailingContent.bottom
                    anchors.topMargin: 5
                    text: row.secondaryValue
                    color: Theme.mutedText
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }

                Rectangle {
                    visible: row.index > 0
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: Theme.border
                }
            }

            onClicked: if (canActivate && rowIdentifier.length > 0) root.activated(rowIdentifier)
        }

        Text {
            anchors.centerIn: parent
            width: Math.max(0, parent.width - 48)
            visible: list.count === 0
            text: root.emptyText
            color: Theme.mutedText
            font.pixelSize: 16
            font.italic: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }
}
