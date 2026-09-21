import QtQuick
import Mouffette.Canvas

BaseMediaItem {
    id: root
    property var residentFrameSource: null
    property bool residencyReady: false
    property bool presentationReady: residencyReady
    property string loadingState: ""
    property string loadingError: ""
    property bool requireInitialSkeleton: true
    contentReady: root.presentationReady && !!imageLoader.item && imageLoader.item.hasFrame
    initialFramePresented: surface.renderingAllowed

    MediaSurface {
        id: surface
        anchors.fill: parent
        requireInitialSkeleton: root.requireInitialSkeleton
        residencyReady: root.presentationReady
        contentReady: root.contentReady
        loadingState: root.loadingState
        loadingError: root.loadingError
        Loader {
            id: imageLoader
            anchors.fill: parent
            // Create the source-sized texture only once a resident frame can
            // be presented, after the initial loading skeleton.
            active: root.residencyReady && !!root.residentFrameSource
                    && root.residentFrameSource.hasFrame === true
                    && surface.renderingAllowed
            sourceComponent: RemoteVideoFrameItem {
                frameSource: root.residentFrameSource
            }
        }
    }
}
