import QtQuick
import Mouffette.Canvas

BaseMediaItem {
    id: root
    property var residentFrameSource: null
    property bool residencyReady: false
    property bool requireInitialSkeleton: true
    contentReady: root.residencyReady && !!imageLoader.item && imageLoader.item.hasFrame
    initialFramePresented: !requireInitialSkeleton || surface.firstFramePresented

    MediaSurface {
        id: surface
        anchors.fill: parent
        requireInitialSkeleton: root.requireInitialSkeleton
        contentReady: root.contentReady
        Loader {
            id: imageLoader
            anchors.fill: parent
            // A hidden painted item still allocates its backing surface during
            // scene-graph synchronization. Create it only for a resident frame.
            active: root.residencyReady && !!root.residentFrameSource
                    && root.residentFrameSource.hasFrame === true
                    && (!root.requireInitialSkeleton || surface.firstFramePresented)
            sourceComponent: RemoteVideoFrameItem {
                frameSource: root.residentFrameSource
            }
        }
    }
}
