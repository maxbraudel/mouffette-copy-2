import QtQuick
import QtQuick.Controls
import Mouffette.App
import "../components"

AppPanel {
    id: root

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

            width: list.width
            height: secondaryValue.length > 0 ? 66 : 48
            enabled: canActivate
            hoverEnabled: true
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
                    anchors.right: projectBadge.left
                    anchors.rightMargin: 10
                    anchors.top: parent.top
                    anchors.topMargin: 7
                    height: 22
                    text: row.primaryValue
                    color: Theme.text
                    font.weight: Font.DemiBold
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                Rectangle {
                    id: projectBadge
                    visible: row.ongoingProject
                    anchors.right: badge.left
                    anchors.rightMargin: visible ? 8 : 0
                    anchors.top: parent.top
                    anchors.topMargin: 7
                    width: visible ? projectBadgeLabel.implicitWidth + 18 : 0
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
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.top: parent.top
                    anchors.topMargin: 7
                    width: Math.max(54, badgeLabel.implicitWidth + 18)
                    height: 22
                    radius: height / 2
                    color: row.availableBadge ? Theme.buttonBackground
                           : row.badgeKindValue === 0 ? Theme.connectedBackground
                           : row.badgeKindValue === 1 ? Theme.warningBackground
                                                     : Theme.errorBackground

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

                Text {
                    visible: text.length > 0
                    anchors.left: primary.left
                    anchors.right: badge.right
                    anchors.top: badge.bottom
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
