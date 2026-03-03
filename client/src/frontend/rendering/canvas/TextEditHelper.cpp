#include "frontend/rendering/canvas/TextEditHelper.h"

#include <QAbstractTextDocumentLayout>
#include <QPointer>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextOption>

// Qt private header: gives us QTextEngine and QScriptLine with the
// textAdvance / textWidth / hasTrailingSpaces fields used below.
#include <QtGui/private/qtextengine_p.h>

TextEditHelper::TextEditHelper(QObject* parent)
    : QObject(parent)
{
}

// -----------------------------------------------------------------------
// fixTextAdvancesForTrailingSpaces
//
// Root-cause analysis (Qt 6.9 source, qtextlayout.cpp layout_helper):
//
//   found:
//     line.textAdvance = line.textWidth;        // ← set FIRST
//     ...
//     const QFixed trailingSpace =
//         (includeTrailingSpaces ? lbh.spaceData.textWidth : QFixed(0));
//     line.textWidth += trailingSpace;           // ← added AFTER textAdvance
//
// QTextEngine::alignLine() computes centering as:
//   x = (line.width - line.textAdvance) / 2
//
// Because textAdvance is assigned before trailing spaces are added to
// textWidth, it never contains the trailing-space contribution regardless
// of the QTextOption::IncludeTrailingSpaces flag.
//
// Fix: after every layout pass, overwrite textAdvance with the already-
// corrected textWidth (which includes trailing spaces when
// IncludeTrailingSpaces is set) for any line that has trailing spaces.
// This runs synchronously inside the QAbstractTextDocumentLayout::update
// signal handler, before Qt's deferred repaint consumes the value.
// -----------------------------------------------------------------------
static void fixTextAdvancesForTrailingSpaces(QTextDocument* doc)
{
    for (QTextBlock block = doc->begin(); block != doc->end(); block = block.next()) {
        QTextLayout* tl = block.layout();
        if (!tl)
            continue;
        QTextEngine* engine = tl->engine();
        if (!engine)
            continue;
        for (int i = 0; i < engine->lines.size(); ++i) {
            QScriptLine& sl = engine->lines[i];
            if (sl.hasTrailingSpaces) {
                // textWidth already includes the trailing-space width
                // (IncludeTrailingSpaces is set on the document option).
                // alignLine() uses textAdvance for centering — sync them.
                sl.textAdvance = sl.textWidth;
            }
        }
    }
}

void TextEditHelper::applyIncludeTrailingSpaces(QObject* obj)
{
    if (!obj)
        return;

    QVariant prop = obj->property("textDocument");
    if (!prop.isValid())
        return;

    auto* quickDoc = qvariant_cast<QQuickTextDocument*>(prop);
    if (!quickDoc)
        return;

    QTextDocument* doc = quickDoc->textDocument();
    if (!doc)
        return;

    // Step 1 — ensure IncludeTrailingSpaces is set so that layout_helper
    // adds the trailing-space width to line.textWidth.  Without this flag,
    // textWidth would also exclude trailing spaces and our fixup below
    // would have nothing to copy.
    QTextOption opt = doc->defaultTextOption();
    if (!(opt.flags() & QTextOption::IncludeTrailingSpaces)) {
        opt.setFlags(opt.flags() | QTextOption::IncludeTrailingSpaces);
        doc->setDefaultTextOption(opt);
        doc->markContentsDirty(0, doc->characterCount());
    }

    // Step 2 — install a persistent post-layout hook.  On every layout pass
    // QAbstractTextDocumentLayout emits update(); we patch textAdvance there.
    // The custom property guards against duplicate connections across multiple
    // calls to applyIncludeTrailingSpaces on the same document.
    if (!doc->property("_trailingSpaceAdvanceFix").toBool()) {
        doc->setProperty("_trailingSpaceAdvanceFix", true);
        QAbstractTextDocumentLayout* dl = doc->documentLayout();
        // Use dl as the context so the connection is auto-removed when the
        // document layout (and thus the document itself) is destroyed.
        // Use QPointer<QTextDocument> so the lambda is safe if doc is ever
        // deleted before dl (unusual but defensive).
        QPointer<QTextDocument> docPtr(doc);
        connect(dl, &QAbstractTextDocumentLayout::update,
                dl, [docPtr]() {
                    if (docPtr)
                        fixTextAdvancesForTrailingSpaces(docPtr);
                });
    }

    // Step 3 — fix any already-laid-out lines immediately so the very next
    // paint frame is correct (the signal fires only on the next layout pass).
    fixTextAdvancesForTrailingSpaces(doc);
}

