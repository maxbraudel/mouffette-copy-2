#pragma once

#include <QObject>
#include <QtQml/qqml.h>

/**
 * TextEditHelper
 *
 * QML singleton that fixes horizontal alignment of lines with trailing spaces
 * in an editable TextEdit.
 *
 * Root cause (Qt 6, qtextlayout.cpp): QTextEngine::alignLine() uses
 * line.textAdvance for centering, but layout_helper sets textAdvance BEFORE
 * adding the trailing-space contribution to textWidth — so textAdvance never
 * includes trailing spaces regardless of QTextOption::IncludeTrailingSpaces.
 *
 * Fix: sets IncludeTrailingSpaces on the document so textWidth includes
 * trailing spaces, then installs a persistent post-layout hook that copies
 * textWidth → textAdvance for every line that has trailing spaces.
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

    // Call once from the TextEdit's Component.onCompleted.  Installs a
    // persistent post-layout hook so every subsequent layout pass is
    // automatically corrected.
    Q_INVOKABLE void applyIncludeTrailingSpaces(QObject* textEdit);
};
