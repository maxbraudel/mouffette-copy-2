#pragma once

#include <QByteArray>
#include <QHash>
#include <QVector>
#include <QString>
#include <cstdint>

// ---------------------------------------------------------------------------
// GlyphBorderAtlas
//
// Singleton that loads a pre-baked glyph border atlas (.gba) and provides
// scaled StrokeGlyphSvg entries for TextGlyphPath::buildStrokeSvg().
//
// The atlas stores stroke paths for every printable character of Impact at
// a reference pixel size (1000 px) and for 20 thickness levels (5%..100%).
// At runtime, coordinates are multiplied by (actualPixelSize / 1000) before
// being formatted into the QByteArray body — making the hot path identical
// to what TextGlyphPath already does, just without QPainterPathStroker.
//
// Usage:
//   GlyphBorderAtlas::instance().load(":/fonts/impact_border_atlas.gba");
//   ...
//   StrokeGlyphSvg svg;
//   if (GlyphBorderAtlas::instance().buildScaledEntry(codepoint, pct, scale, svg))
//       useAtlasResult(svg);
//   else
//       fallbackToStroker();
// ---------------------------------------------------------------------------

// Forward declaration — these structs are defined in TextGlyphPath.h but we
// redeclare them here so GlyphBorderAtlas.h doesn't pull in all of Qt Quick.
struct AtlasSubpathRaw {
    float          ox, oy;         // origin in refPixelSize (1000 px) coordinate space
    QVector<uint8_t> commands;     // 0=LineTo(2 floats), 1=CurveTo(6 floats), 2=Close(0 floats)
    QVector<float>   args;         // matching float arguments, in relative coordinate space
};

class GlyphBorderAtlas
{
public:
    static GlyphBorderAtlas& instance();

    // Load atlas from the given path (Qt resource ":/..." or filesystem path).
    // Returns true if the atlas was loaded and verified successfully.
    // Safe to call multiple times — subsequent calls replace the current state.
    bool load(const QString& path);

    bool    isLoaded()      const { return m_loaded; }
    float   refPixelSize()  const { return m_refPixelSize; }

    // Snap thicknessPercent to the nearest pre-baked atlas step.
    // Returns -1.0f if the atlas is not loaded or the value is out of range.
    float nearestThicknessStep(float thicknessPercent) const;

    // Look up pre-computed stroke subpaths for a Unicode codepoint at the
    // given thickness percentage, and write scaled SVG bodies into |out|.
    //
    // Parameters:
    //   codepoint         — Unicode scalar value of the character
    //   thicknessPercent  — stroke width as % of pixel size (snapped internally)
    //   scale             — actualPixelSize / refPixelSize
    //   out               — vector to clear and populate with raw subpath data
    //
    // Returns true and populates |out| on success.
    // Returns false (|out| is untouched) when: atlas not loaded, codepoint not
    // found, or thickness out of range — caller should fall back to stroker.
    bool lookup(uint32_t codepoint, float thicknessPercent,
                QVector<AtlasSubpathRaw>& out) const;

    // Convenience: returns the number of glyph entries in the atlas.
    int glyphCount()     const { return m_glyphs.size(); }
    int thicknessCount() const { return m_thicknesses.size(); }

private:
    GlyphBorderAtlas() = default;
    GlyphBorderAtlas(const GlyphBorderAtlas&) = delete;
    GlyphBorderAtlas& operator=(const GlyphBorderAtlas&) = delete;

    // ---------------------------------------------------------------------------
    // In-memory representation of a single (codepoint × thickness) entry.
    // ---------------------------------------------------------------------------
    struct AtlasEntry {
        // One element per pre-baked thickness (index matches m_thicknesses[]).
        // An empty QVector means the glyph has no stroke at that thickness
        // (e.g. space character).
        QVector<QVector<AtlasSubpathRaw>> perThickness;
    };

    bool          m_loaded       { false };
    float         m_refPixelSize { 1000.0f };
    QVector<float> m_thicknesses;                   // sorted, e.g. [5.0, 10.0, ..., 100.0]
    QHash<uint32_t, AtlasEntry> m_glyphs;           // glyph index → entry
};
