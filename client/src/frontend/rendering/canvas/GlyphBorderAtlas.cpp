#include "frontend/rendering/canvas/GlyphBorderAtlas.h"

#include <QByteArray>
#include <QDataStream>
#include <QFile>
#include <QDebug>
#include <cmath>

// ---------------------------------------------------------------------------
// .gba binary format (version 1)
//
//  [0-3]   magic:                  char[4] = "GBA1"
//  [4-5]   version:                uint16  = 1
//  [6-7]   thicknessCount:         uint16
//  [8-11]  glyphCount:             uint32
//  [12-15] refPixelSize:           float32
//  [16-23] (reserved / pad)        uint64  = 0
//  [24-27] dataCrc32:              uint32  (CRC32 of the raw uncompressed data blob)
//  [28-31] (reserved)              uint32  = 0
//
// Immediately after header:
//  Thickness table:  float32[thicknessCount]
//
//  Glyph index:  for each glyph:
//    codepoint: uint32
//    For each thickness: subpath data is encoded inline in the data blob.
//
// Data blob (starts immediately, NOT compressed here — qCompress prepends
// an internal length header and we just use qUncompress on the full section):
//  The data blob is a flat QByteArray containing all entries back-to-back.
//  The glyph index stores offsets into this blob (int64, -1 = missing).
//
// Each (glyph × thickness) entry in the blob:
//   subpathCount:  uint16
//   For each subpath:
//     ox:         float32
//     oy:         float32
//     argCount:   uint32   (total number of floats for all commands)
//     cmdCount:   uint32   (number of commands)
//     floats:     float32[argCount]    (relative coords, refPixelSize space)
//     commands:   uint8[cmdCount]      (0=LineTo, 1=CurveTo, 2=Close)
// ---------------------------------------------------------------------------

static constexpr uint32_t kMagic   = 0x31414247u; // "GBA1" as little-endian uint32
static constexpr uint16_t kVersion = 1;

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

GlyphBorderAtlas& GlyphBorderAtlas::instance()
{
    static GlyphBorderAtlas s_instance;
    return s_instance;
}

// ---------------------------------------------------------------------------
// load()
// ---------------------------------------------------------------------------

bool GlyphBorderAtlas::load(const QString& path)
{
    m_loaded = false;
    m_thicknesses.clear();
    m_glyphs.clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[GlyphBorderAtlas] Cannot open:" << path;
        return false;
    }

    QByteArray raw = file.readAll();
    file.close();

    if (raw.size() < 32) {
        qWarning() << "[GlyphBorderAtlas] File too small:" << path;
        return false;
    }

    QDataStream hdr(raw);
    hdr.setByteOrder(QDataStream::LittleEndian);
    hdr.setFloatingPointPrecision(QDataStream::SinglePrecision);

    // ── Header ────────────────────────────────────────────────────────────────
    uint32_t magic = 0;
    hdr >> magic;
    if (magic != kMagic) {
        qWarning() << "[GlyphBorderAtlas] Bad magic in:" << path;
        return false;
    }

    uint16_t version = 0, thicknessCount = 0;
    uint32_t glyphCount = 0;
    float    refPixelSize = 0.0f;
    uint64_t reserved = 0;
    uint32_t crc32 = 0, reserved2 = 0;

    hdr >> version >> thicknessCount >> glyphCount
        >> refPixelSize >> reserved >> crc32 >> reserved2;

    if (version != kVersion) {
        qWarning() << "[GlyphBorderAtlas] Unsupported version" << version << "in:" << path;
        return false;
    }
    if (thicknessCount == 0 || glyphCount == 0 || refPixelSize <= 0.0f) {
        qWarning() << "[GlyphBorderAtlas] Invalid header in:" << path;
        return false;
    }

    // ── Thickness table ───────────────────────────────────────────────────────
    m_thicknesses.resize(thicknessCount);
    for (int i = 0; i < thicknessCount; ++i)
        hdr >> m_thicknesses[i];

    // ── Glyph index ───────────────────────────────────────────────────────────
    // Each glyph entry: codepoint(uint32) + int64 offset per thickness.
    // Offset is -1 if the glyph has no stroke at that thickness.
    struct IndexEntry { uint32_t glyphIdx; QVector<int64_t> offsets; };
    QVector<IndexEntry> index(glyphCount);
    for (uint32_t g = 0; g < glyphCount; ++g) {
        hdr >> index[g].glyphIdx;
        index[g].offsets.resize(thicknessCount);
        for (int t = 0; t < thicknessCount; ++t) {
            int64_t off = 0;
            hdr >> off;
            index[g].offsets[t] = off;
        }
    }

    if (hdr.status() != QDataStream::Ok) {
        qWarning() << "[GlyphBorderAtlas] Read error in header/index of:" << path;
        return false;
    }

    // ── Data blob (qCompress-compressed) ─────────────────────────────────────
    // Everything remaining in the file is the compressed data blob.
    const int indexEnd = static_cast<int>(hdr.device()->pos());
    QByteArray compressedBlob = raw.mid(indexEnd);
    QByteArray blob = qUncompress(compressedBlob);
    if (blob.isEmpty()) {
        qWarning() << "[GlyphBorderAtlas] Failed to decompress data blob in:" << path;
        return false;
    }

    // ── Deserialize each entry from the blob ─────────────────────────────────
    QDataStream ds(blob);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::SinglePrecision);

    // We don't use random-access offsets here; instead the generator writes
    // entries in the same deterministic order as the index (glyph × thickness).
    // We just read them in that order.
    m_glyphs.reserve(glyphCount);

    for (uint32_t g = 0; g < glyphCount; ++g) {
        AtlasEntry entry;
        entry.perThickness.resize(thicknessCount);

        for (int t = 0; t < thicknessCount; ++t) {
            if (index[g].offsets[t] < 0) {
                // Missing — leave empty
                continue;
            }

            uint16_t subpathCount = 0;
            ds >> subpathCount;

            QVector<AtlasSubpathRaw>& subpaths = entry.perThickness[t];
            subpaths.resize(subpathCount);

            for (uint16_t s = 0; s < subpathCount; ++s) {
                AtlasSubpathRaw& sp = subpaths[s];
                uint32_t argCount = 0, cmdCount = 0;
                ds >> sp.ox >> sp.oy >> argCount >> cmdCount;

                sp.args.resize(argCount);
                for (uint32_t a = 0; a < argCount; ++a)
                    ds >> sp.args[a];

                sp.commands.resize(cmdCount);
                for (uint32_t c = 0; c < cmdCount; ++c) {
                    uint8_t cmd = 0;
                    ds >> cmd;
                    sp.commands[c] = cmd;
                }
            }
        }

        m_glyphs.insert(index[g].glyphIdx, std::move(entry));
    }

    if (ds.status() != QDataStream::Ok) {
        qWarning() << "[GlyphBorderAtlas] Data blob read error in:" << path;
        m_glyphs.clear();
        m_thicknesses.clear();
        return false;
    }

    m_refPixelSize = refPixelSize;
    m_loaded       = true;
    qDebug() << "[GlyphBorderAtlas] Loaded:" << glyphCount << "glyphs x"
             << thicknessCount << "thicknesses from" << path;
    return true;
}

// ---------------------------------------------------------------------------
// nearestThicknessStep()
// ---------------------------------------------------------------------------

float GlyphBorderAtlas::nearestThicknessStep(float thicknessPercent) const
{
    if (!m_loaded || m_thicknesses.isEmpty())
        return -1.0f;

    if (thicknessPercent <= m_thicknesses.first())
        return m_thicknesses.first();
    if (thicknessPercent >= m_thicknesses.last())
        return m_thicknesses.last();

    // Linear scan — 20 entries max, negligible cost
    float bestDiff  = qAbs(thicknessPercent - m_thicknesses[0]);
    float bestValue = m_thicknesses[0];
    for (int i = 1; i < m_thicknesses.size(); ++i) {
        float diff = qAbs(thicknessPercent - m_thicknesses[i]);
        if (diff < bestDiff) {
            bestDiff  = diff;
            bestValue = m_thicknesses[i];
        }
    }
    return bestValue;
}

// ---------------------------------------------------------------------------
// lookup()
// ---------------------------------------------------------------------------

bool GlyphBorderAtlas::lookup(uint32_t glyphIndex, float thicknessPercent,
                               QVector<AtlasSubpathRaw>& out) const
{
    if (!m_loaded)
        return false;

    auto glyphIt = m_glyphs.constFind(glyphIndex);
    if (glyphIt == m_glyphs.constEnd())
        return false;

    // Snap to nearest atlas thickness step
    float snapped  = nearestThicknessStep(thicknessPercent);
    if (snapped < 0.0f)
        return false;

    // Find index of snapped thickness
    int thicknessIdx = -1;
    float bestDiff   = 1e9f;
    for (int i = 0; i < m_thicknesses.size(); ++i) {
        float d = qAbs(m_thicknesses[i] - snapped);
        if (d < bestDiff) { bestDiff = d; thicknessIdx = i; }
    }
    if (thicknessIdx < 0)
        return false;

    const QVector<AtlasSubpathRaw>& subpaths =
        glyphIt->perThickness[thicknessIdx];

    // An empty subpath list for a non-space character means the glyph was
    // not present in the font — tell the caller to skip this glyph.
    out = subpaths;
    return true;
}
