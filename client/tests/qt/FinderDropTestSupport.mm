#import <Cocoa/Cocoa.h>
#include <QWindow>
#include <QPoint>
#include <QString>
#include <QtTest>

// Exercise Qt's Cocoa MIME conversion without using the general clipboard.
// WindowServer's level restriction is checked separately in tst_WindowPresentation;
// directly invoking these callbacks cannot validate OS drag destination routing.
@interface FinderDropTestInfo : NSObject
@property(nonatomic, retain) NSPasteboard* draggingPasteboard;
@property(nonatomic) NSPoint draggingLocation;
@end
@implementation FinderDropTestInfo
- (NSDragOperation)draggingSourceOperationMask { return NSDragOperationCopy | NSDragOperationMove | NSDragOperationLink; }
- (id)draggingSource { return nil; }
@end

bool performFinderDrop(QWindow* window, const QString& path, const QPoint& position)
{
    @autoreleasepool {
        NSView* view = reinterpret_cast<NSView*>(window->winId());
        NSPasteboard* board = [NSPasteboard pasteboardWithUniqueName];
        NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
        [board writeObjects:@[url]];
        FinderDropTestInfo* info = [[FinderDropTestInfo alloc] init];
        info.draggingPasteboard = board;
        info.draggingLocation = [view convertPoint:NSMakePoint(position.x(), position.y()) toView:nil];
        id<NSDraggingInfo> sender = (id<NSDraggingInfo>)info;
        const auto entered = [view draggingEntered:sender];
        const auto updated = [view draggingUpdated:sender];
        QTest::qWait(650); // include a production priority-enforcement tick
        const bool dropped = [view performDragOperation:sender];
        [board releaseGlobally];
        info.draggingPasteboard = nil;
        [info release];
        return entered == NSDragOperationCopy && updated == NSDragOperationCopy && dropped;
    }
}
