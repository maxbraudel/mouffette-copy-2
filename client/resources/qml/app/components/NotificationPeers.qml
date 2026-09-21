import QtQuick
import QtQuick.Layouts
import Mouffette.App

ColumnLayout {
    id: root
    property var controller: null
    property var peers: []
    property color textColor: Theme.text
    spacing: 5
    visible: peers.length > 0

    Repeater {
        model: root.peers
        delegate: RowLayout {
            id: peerRow
            required property var modelData
            Layout.fillWidth: true
            spacing: 7

            Text {
                visible: peerRow.modelData.role.length > 0
                text: peerRow.modelData.role
                color: root.textColor
                font.pixelSize: 12
            }
            ProfilePicture {
                controller: root.controller
                endpointId: peerRow.modelData.endpointId
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
            }
            Text {
                Layout.fillWidth: true
                text: {
                    const revision = root.controller ? root.controller.profileRevision : 0
                    if (root.controller)
                        return root.controller.clientDisplayName(peerRow.modelData.endpointId,
                            peerRow.modelData.machineName, peerRow.modelData.instanceOrdinal)
                    return (peerRow.modelData.machineName || peerRow.modelData.endpointId)
                        + " (" + peerRow.modelData.instanceOrdinal + ")"
                }
                textFormat: Text.PlainText
                color: root.textColor
                font.pixelSize: 13
                font.bold: true
                elide: Text.ElideRight
            }
        }
    }
}
