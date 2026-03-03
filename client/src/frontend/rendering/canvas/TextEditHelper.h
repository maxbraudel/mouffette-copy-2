#pragma once

#include <QObject>
#include <QtQml/qqml.h>

/**
 * TextEditHelper
 *
 * QML singleton that patches a TextEdit's underlying QTextDocument to include
 * trailing spaces when computing line alignment widths.
 *
 * By default Qt's QTextDocument uses QTextLine::naturalTextWidth() for
 * horizontal alignment, which STRIPS trailing whitespace.  This means a line
 * like "hello   " (with trailing spaces) is centred/right-aligned as if those
 * spaces don't exist — causing a visible left-shift versus a read-only TextEdit
 * (which internally sets QTextOption::IncludeTrailingSpaces).
 *
 * Call applyIncludeTrailingSpaces(this) in QML from the TextEdit's
 * Component.onCompleted handler to make its document behave identically to the
 * display-mode node.
 *
 * QML usage:
 *   import Mouffette.Canvas 1.0
 *   TextEdit {
 *       Component.onCompleted: TextEditHelper.applyIncludeTrailingSpaces(this)
 *   }
 */
class TextEditHelper : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit TextEditHelper(QObject* parent = nullptr);

    // Parameter is QObject* (not QQuickItem*) so MOC can register it as a
    // complete metatype without pulling in QtQuick private headers.  The
    // implementation casts via qobject_cast<QQuickItem*> internally.
    Q_INVOKABLE void applyIncludeTrailingSpaces(QObject* item);

    // Returns the horizontal pixel offset needed to compensate alignment in
    // editable TextEdit when trailing spaces on the first line are ignored by
    // the internal layout's naturalTextWidth() logic.
    //
    // For center alignment: offset = trailingWidth / 2
    // For right alignment:  offset = trailingWidth
    // For left alignment:   offset = 0
    Q_INVOKABLE qreal trailingSpaceAlignmentOffset(QObject* item) const;
};
