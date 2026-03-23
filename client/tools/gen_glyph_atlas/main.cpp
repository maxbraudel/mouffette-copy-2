// gen_glyph_atlas — standalone Qt console tool
//
// Generates a .gba (Glyph Border Atlas) file from a TrueType/OpenType font.
// The atlas pre-bakes stroke paths for every requested codepoint at multiple
// thickness percentages and a fixed reference pixel size.  The main app loads
// this file at startup to avoid running QPainterPathStroker at runtime.
//
// Usage:
//   gen_glyph_atlas --font <path.ttf> --output <path.gba>
//                   [--ref-size 1000]
//                   [--chars "32-126,160-255"]
//                   [--steps "5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100"]
//
// Output format: see GlyphBorderAtlas.cpp for binary layout description.

#include <QGuiApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QDataStream>
#include <QByteArray>
#include <QDebug>
#include <QRawFont>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QVector>
#include <QString>
#include <QStringList>
#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------------------
// Shared geometry helpers (mirrors TextGlyphPath.cpp logic exactly)
// ---------------------------------------------------------------------------

// Extract the subpath with the largest area (the outer silhouette of counter
// glyphs like O, A, B — same logic as in TextGlyphPath.cpp).
static QPainterPath extractLargestSubpath(const QPainterPath& path)
{
    const int total = path.elementCount();
    if (total == 0) return path;

    QVector<int> starts;
    for (int i = 0; i < total; ++i)
        if (path.elementAt(i).type == QPainterPath::MoveToElement)
            starts.append(i);
    if (starts.size() <= 1) return path;

    int   bestIdx  = 0;
    qreal bestArea = 0.0;
    for (int si = 0; si < starts.size(); ++si) {
        const int from = starts[si];
        const int to   = (si + 1 < starts.size()) ? starts[si + 1] - 1 : total - 1;
        qreal area = 0.0, px = path.elementAt(from).x, py = path.elementAt(from).y;
        for (int ei = from + 1; ei <= to; ++ei) {
            qreal cx = path.elementAt(ei).x, cy = path.elementAt(ei).y;
            area += px * cy - cx * py;
            px = cx; py = cy;
        }
        area = qAbs(area) * 0.5;
        if (area > bestArea) { bestArea = area; bestIdx = si; }
    }

    const int from = starts[bestIdx];
    const int to   = (bestIdx + 1 < starts.size()) ? starts[bestIdx + 1] - 1 : total - 1;
    QPainterPath result;
    result.setFillRule(path.fillRule());
    for (int ei = from; ei <= to; ) {
        const QPainterPath::Element& el = path.elementAt(ei);
        switch (el.type) {
        case QPainterPath::MoveToElement:  result.moveTo(el.x, el.y); ++ei; break;
        case QPainterPath::LineToElement:  result.lineTo(el.x, el.y); ++ei; break;
        case QPainterPath::CurveToElement: {
            const auto& cp2 = path.elementAt(ei+1);
            const auto& ep  = path.elementAt(ei+2);
            result.cubicTo(el.x,el.y, cp2.x,cp2.y, ep.x,ep.y);
            ei += 3; break;
        }
        default: ++ei; break;
        }
    }
    result.closeSubpath();
    return result;
}

// ---------------------------------------------------------------------------
// Data types used for serialization — match GlyphBorderAtlas.h
// ---------------------------------------------------------------------------

struct SubpathData {
    float            ox, oy;
    QVector<float>   args;      // relative coords in reference pixel space
    QVector<uint8_t> commands;  // 0=LineTo, 1=CurveTo, 2=Close
};

// Converts a stroked QPainterPath into a SubpathData list using relative coords.
static QVector<SubpathData> buildSubpaths(const QPainterPath& path)
{
    QVector<SubpathData> result;
    const int n = path.elementCount();
    if (n == 0) return result;

    SubpathData* cur = nullptr;
    float cx = 0.0f, cy = 0.0f;

    for (int i = 0; i < n; ) {
        const QPainterPath::Element& el = path.elementAt(i);
        switch (el.type) {
        case QPainterPath::MoveToElement:
            if (cur && !cur->commands.isEmpty() && cur->commands.last() != 2)
                cur->commands.append(2); // Close
            result.append(SubpathData{});
            cur = &result.last();
            cur->ox = static_cast<float>(el.x);
            cur->oy = static_cast<float>(el.y);
            cx = cur->ox; cy = cur->oy;
            ++i;
            break;
        case QPainterPath::LineToElement:
            if (cur) {
                cur->commands.append(0); // LineTo
                cur->args.append(static_cast<float>(el.x) - cx);
                cur->args.append(static_cast<float>(el.y) - cy);
                cx = static_cast<float>(el.x);
                cy = static_cast<float>(el.y);
            }
            ++i;
            break;
        case QPainterPath::CurveToElement:
            if (cur && i + 2 < n) {
                const auto& cp2 = path.elementAt(i+1);
                const auto& ep  = path.elementAt(i+2);
                cur->commands.append(1); // CurveTo
                cur->args.append(static_cast<float>(el.x)  - cx);
                cur->args.append(static_cast<float>(el.y)  - cy);
                cur->args.append(static_cast<float>(cp2.x) - cx);
                cur->args.append(static_cast<float>(cp2.y) - cy);
                cur->args.append(static_cast<float>(ep.x)  - cx);
                cur->args.append(static_cast<float>(ep.y)  - cy);
                cx = static_cast<float>(ep.x);
                cy = static_cast<float>(ep.y);
            }
            i += 3;
            break;
        default:
            ++i;
            break;
        }
    }
    if (cur && !cur->commands.isEmpty() && cur->commands.last() != 2)
        cur->commands.append(2); // Close

    // Remove empty subpaths
    result.erase(std::remove_if(result.begin(), result.end(),
        [](const SubpathData& s){ return s.commands.isEmpty(); }), result.end());

    return result;
}

// ---------------------------------------------------------------------------
// Argument parsing helpers
// ---------------------------------------------------------------------------

static QVector<uint32_t> parseCharRanges(const QString& spec)
{
    // "32-126,160-255,8364" → sorted list of codepoints
    QVector<uint32_t> codepoints;
    for (const QString& part : spec.split(u',')) {
        const QString p = part.trimmed();
        const int dashPos = p.indexOf(u'-', 1); // skip leading '-' for negatives
        if (dashPos > 0) {
            bool okA, okB;
            uint32_t a = p.left(dashPos).toUInt(&okA);
            uint32_t b = p.mid(dashPos+1).toUInt(&okB);
            if (okA && okB && a <= b)
                for (uint32_t c = a; c <= b; ++c) codepoints.append(c);
        } else {
            bool ok;
            uint32_t c = p.toUInt(&ok);
            if (ok) codepoints.append(c);
        }
    }
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    return codepoints;
}

static QVector<float> parseSteps(const QString& spec)
{
    QVector<float> steps;
    for (const QString& part : spec.split(u',')) {
        bool ok;
        float v = part.trimmed().toFloat(&ok);
        if (ok && v > 0.0f && v <= 100.0f) steps.append(v);
    }
    std::sort(steps.begin(), steps.end());
    steps.erase(std::unique(steps.begin(), steps.end()), steps.end());
    return steps;
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("gen_glyph_atlas");
    app.setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Pre-bake glyph border stroke paths into a .gba atlas file.\n"
        "Run ./scripts/gen_impact_atlas.sh for the default Impact font atlas.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"font",     "Input font file (.ttf/.otf)",                 "path"});
    parser.addOption({"output",   "Output .gba file path",                       "path"});
    parser.addOption({"ref-size", "Reference pixel size (default: 1000)",        "px",   "1000"});
    parser.addOption({"chars",    "Codepoint ranges (default: 32-126,160-255)", "spec", "32-126,160-255"});
    parser.addOption({"steps",    "Thickness percentages (default: 5,10,...,100)", "list",
                      "5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,100"});
    parser.process(app);

    const QString fontPath   = parser.value("font");
    const QString outputPath = parser.value("output");
    const float   refSize    = parser.value("ref-size").toFloat();
    const QVector<uint32_t> codepoints = parseCharRanges(parser.value("chars"));
    const QVector<float>    steps      = parseSteps(parser.value("steps"));

    if (fontPath.isEmpty() || outputPath.isEmpty()) {
        qCritical() << "Usage: gen_glyph_atlas --font <path.ttf> --output <path.gba>";
        return 1;
    }
    if (refSize <= 0.0f)  { qCritical() << "Invalid --ref-size"; return 1; }
    if (codepoints.isEmpty()) { qCritical() << "No codepoints specified"; return 1; }
    if (steps.isEmpty())  { qCritical() << "No thickness steps specified"; return 1; }

    qInfo() << "Loading font:" << fontPath << "at" << refSize << "px";
    QRawFont rawFont = QRawFont(fontPath, refSize, QFont::PreferNoHinting);
    if (!rawFont.isValid()) {
        qCritical() << "Failed to load font:" << fontPath;
        return 1;
    }
    qInfo() << "Font family:" << rawFont.familyName()
            << " Style:" << rawFont.styleName()
            << " Pixel size:" << rawFont.pixelSize();

    // ── Map codepoints → unique glyph indexes ────────────────────────────────
    // The atlas is keyed by font glyph index (not Unicode codepoint), so
    // multiple codepoints that map to the same glyph are processed once.
    QVector<uint32_t> glyphIds;        // unique, sorted glyph indexes
    {
        QSet<uint32_t> seen;
        for (uint32_t cp : codepoints) {
            const char32_t ch32   = static_cast<char32_t>(cp);
            const QString   cpStr  = QString::fromUcs4(&ch32, 1);
            const QVector<quint32> gi = rawFont.glyphIndexesForString(cpStr);
            if (gi.isEmpty() || gi[0] == 0) continue; // .notdef or unmapped
            if (!seen.contains(gi[0])) {
                seen.insert(gi[0]);
                glyphIds.append(gi[0]);
            }
        }
        std::sort(glyphIds.begin(), glyphIds.end());
    }
    qInfo() << "Unique glyph indexes to process:" << glyphIds.size()
            << "(from" << codepoints.size() << "requested codepoints)";

    // ── Build atlas data ──────────────────────────────────────────────────────
    const int G = glyphIds.size();
    const int T = steps.size();

    // Per-glyph data: data[g][t] = SubpathData list
    QVector<QVector<QVector<SubpathData>>> data(G, QVector<QVector<SubpathData>>(T));
    QVector<QVector<int64_t>> offsets(G, QVector<int64_t>(T, -1LL));

    int processed = 0, skipped = 0;
    int64_t blobOffset = 0;

    // Glyph fill path cache (outer contour, refPixelSize space)
    QHash<uint32_t, QPainterPath> glyphPathCache;

    for (int g = 0; g < G; ++g) {
        const uint32_t glyphId = glyphIds[g];

        // Get outer contour path for this glyph index
        QPainterPath& glyphPath = glyphPathCache[glyphId];
        if (glyphPath.isEmpty()) {
            glyphPath = rawFont.pathForGlyph(glyphId);
            if (!glyphPath.isEmpty())
                glyphPath = extractLargestSubpath(glyphPath);
        }

        if (glyphPath.isEmpty()) {
            ++skipped;
            continue; // space, combining marks, etc. — no visible stroke
        }

        for (int t = 0; t < T; ++t) {
            // strokeWidth = 2 × (thicknessPercent/100) × refPixelSize
            // Mirrors the main app: stroker.setWidth(m_outlinePixels * 2.0)
            // where m_outlinePixels = round(pct × pixelSize / 100).
            const float strokeWidth = 2.0f * (steps[t] / 100.0f) * refSize;

            QPainterPathStroker stroker;
            stroker.setWidth(strokeWidth);
            stroker.setJoinStyle(Qt::RoundJoin);
            stroker.setCapStyle(Qt::RoundCap);
            stroker.setMiterLimit(1.5);
            QPainterPath expanded = stroker.createStroke(glyphPath);

            data[g][t] = buildSubpaths(expanded);
            offsets[g][t] = blobOffset;

            blobOffset += sizeof(uint16_t); // subpathCount
            for (const SubpathData& sp : data[g][t]) {
                blobOffset += sizeof(float) * 2;     // ox, oy
                blobOffset += sizeof(uint32_t) * 2;  // argCount, cmdCount
                blobOffset += sizeof(float)   * sp.args.size();
                blobOffset += sizeof(uint8_t) * sp.commands.size();
            }
        }

        ++processed;
        if (processed % 20 == 0 || processed <= 5)
            qInfo() << "  [" << processed << "/" << G << "] glyph id" << glyphId;
    }

    qInfo() << "Processed:" << processed << " Empty (skipped):" << skipped
            << " Thickness levels:" << T;

    // ── Serialize data blob ───────────────────────────────────────────────────
    QByteArray blob;
    blob.reserve(static_cast<int>(blobOffset));
    {
        QDataStream ds(&blob, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds.setFloatingPointPrecision(QDataStream::SinglePrecision);

        for (int g = 0; g < G; ++g) {
            for (int t = 0; t < T; ++t) {
                if (offsets[g][t] < 0) continue;
                const QVector<SubpathData>& subs = data[g][t];
                ds << static_cast<uint16_t>(subs.size());
                for (const SubpathData& sp : subs) {
                    ds << sp.ox << sp.oy
                       << static_cast<uint32_t>(sp.args.size())
                       << static_cast<uint32_t>(sp.commands.size());
                    for (float f : sp.args)     ds << f;
                    for (uint8_t c : sp.commands) ds << c;
                }
            }
        }
    }

    QByteArray compressed = qCompress(blob, 9);
    qInfo() << "Data blob: uncompressed" << blob.size() / 1024 << "KB, compressed"
            << compressed.size() / 1024 << "KB";

    // ── Write .gba file ───────────────────────────────────────────────────────
    QFile out(outputPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCritical() << "Cannot write output:" << outputPath;
        return 1;
    }

    QDataStream ws(&out);
    ws.setByteOrder(QDataStream::LittleEndian);
    ws.setFloatingPointPrecision(QDataStream::SinglePrecision);

    // Header
    ws << static_cast<uint32_t>(0x31414247u); // magic "GBA1"
    ws << static_cast<uint16_t>(1);           // version
    ws << static_cast<uint16_t>(T);           // thicknessCount
    ws << static_cast<uint32_t>(G);           // glyphCount (unique glyph indexes)
    ws << refSize;                            // refPixelSize
    ws << static_cast<uint64_t>(0);           // reserved
    ws << static_cast<uint32_t>(0);           // crc32 (reserved, not verified at runtime)
    ws << static_cast<uint32_t>(0);           // reserved

    // Thickness table
    for (float s : steps) ws << s;

    // Glyph index: write glyph ID (font-level index, not Unicode codepoint)
    for (int g = 0; g < G; ++g) {
        ws << static_cast<uint32_t>(glyphIds[g]);
        for (int t = 0; t < T; ++t)
            ws << static_cast<int64_t>(offsets[g][t]);
    }

    // Compressed data blob
    out.write(compressed);
    out.close();

    qInfo() << "Written:" << outputPath
            << "(" << (out.size() / 1024) << "KB )";
    return 0;
}
