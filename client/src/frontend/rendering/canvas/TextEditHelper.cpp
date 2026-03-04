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
// QTextEngine::alignLine() computes line position as:
//   center: x = (line.width - line.textAdvance) / 2
//   right:  x = line.width - line.textAdvance
//
// Because textAdvance is assigned before trailing spaces are appended to
// textWidth, it never contains the trailing-space contribution regardless
// of the QTextOption::IncludeTrailingSpaces flag.
//
// For LEFT-aligned text this matters for cursor hit-testing: clicking just
// after a trailing space at a wrapped line end should land in that line, but
// Qt uses textAdvance as the line's effective width for hit-testing purposes.
// Fix: after every layout pass, overwrite textAdvance with textWidth (which
// includes trailing spaces) for left-aligned lines that have trailing spaces.
//
// For CENTER and RIGHT alignment the default textAdvance (without trailing
// spaces) already produces correct visual positioning.  Patching it would
// inflate the "text width" used by alignLine(), shifting the visible text
// inward — producing a horizontal offset between display and edit mode.
// Those alignments are intentionally skipped.
// -----------------------------------------------------------------------
static void fixTextAdvancesForTrailingSpaces(QTextDocument* doc)
{
    for (QTextBlock block = doc->begin(); block != doc->end(); block = block.next()) {
        // Determine effective horizontal alignment.
        //
        // In a QML TextEdit, horizontalAlignment sets the *document-level*
        // default text option (doc->defaultTextOption()), NOT the per-block
        // QTextBlockFormat.  The block format alignment is always Qt::AlignLeft
        // (= 0x0001, which is non-zero/truthy) regardless of the QML property.
        // Therefore we must NOT use blockAlign as a boolean guard — we need to
        // check whether the block has an *explicit* non-left override, and only
        // then prefer it over the document-level setting.
        const Qt::Alignment docAlign =
            doc->defaultTextOption().alignment() & Qt::AlignHorizontal_Mask;
        const Qt::Alignment rawBlockAlign =
            block.blockFormat().alignment() & Qt::AlignHorizontal_Mask;
        // Only treat the block's alignment as a real override if it differs
        // from the default left-alignment that Qt always stamps onto blocks.
        const Qt::Alignment effectiveAlign =
            (rawBlockAlign != Qt::AlignLeft) ? rawBlockAlign : docAlign;

        // For center and right alignment, Qt's default textAdvance already
        // excludes trailing-space width, which is exactly what alignLine()
        // needs to position lines correctly:
        //   center: x = (lineWidth - textAdvance) / 2
        //   right:  x = lineWidth - textAdvance
        // Overwriting textAdvance with the larger textWidth (which includes
        // trailing spaces) would subtract more from the available space and
        // shift the visible text inward (left for center, toward left for
        // right), producing the visible offset between display and edit mode.
        // Only apply the patch for left-aligned text, where the trailing-space
        // advance is needed for correct cursor hit-testing at line ends.
        if (effectiveAlign == Qt::AlignHCenter || effectiveAlign == Qt::AlignRight)
            continue;

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
                // alignLine() uses textAdvance for cursor hit-testing — sync them.
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

