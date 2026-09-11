import QtQuick 2.15
import Mouffette.Canvas 1.0

Item {
    id: root
    objectName: "mediaDropPreview"

    property var preview: ({})
    property var frameSource: null

    readonly property bool shown: !!preview && preview.visible === true
    readonly property bool frameReady: preview && preview.frameReady === true
    readonly property bool handoffActive: !!preview
                                          && !!preview.handoffMediaId

    x: preview && preview.x !== undefined ? preview.x : 0
    y: preview && preview.y !== undefined ? preview.y : 0
    width: preview && preview.width !== undefined ? Math.max(1, preview.width) : 1
    height: preview && preview.height !== undefined ? Math.max(1, preview.height) : 1
    z: 98000
    opacity: shown ? 1.0 : 0.0
    visible: opacity > 0.001
    enabled: false

    Behavior on opacity {
        // Enter/leave keeps the short affordance fade. A successful drop is a
        // visual identity handoff, not a disappearance: once the final media
        // has crossed the render barrier this item is removed atomically.
        enabled: !root.handoffActive
        NumberAnimation {
            duration: 80
            easing.type: Easing.OutCubic
        }
    }

    MediaSurface {
        objectName: "dropPreviewSurface"
        anchors.fill: parent
        contentReady: root.frameReady && previewFrame.hasFrame
        fadeDuration: 80

        RemoteVideoFrameItem {
            id: previewFrame
            anchors.fill: parent
            frameSource: root.frameSource
        }
    }
}
