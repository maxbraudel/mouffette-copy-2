import QtQuick
import QtQuick.Layouts
import Mouffette.App
import "../components"

Item {
    id: root

    required property var controller

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.innerGap

        ClientListPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            controller: root.controller
            model: root.controller.clientsModel
            detailProvider: endpoint => root.controller.clientConnectionDetail(endpoint)
            emptyText: "No clients connected. Make sure other devices are running Mouffette and connected to the same server."
            onActivated: function(identifier) { root.controller.openClient(identifier) }
        }

        Text {
            text: "Ongoing Scenes"
            color: Theme.text
            font.pixelSize: Theme.titleFontSize
            font.bold: true
            Layout.preferredHeight: Theme.titleHeight
            verticalAlignment: Text.AlignVCenter
        }

        ClientListPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            controller: root.controller
            model: root.controller.sceneActivitiesModel
            sceneMode: true
            emptyText: "No current ongoing scenes."
            onActivated: function(identifier) { root.controller.openOngoingScene(identifier) }
        }
    }
}
