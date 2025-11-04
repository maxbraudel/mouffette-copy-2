# Remote Text Border Cropping Issue - Deep Analysis

## Problem Statement
Text items with thick borders (fit-to-text enabled) display correctly on the canvas but show cropped/truncated borders on the remote client during scene playback. The issue is particularly noticeable with fade-in or display delays, but works correctly with immediate auto-display.

## Root Cause Analysis

### 1. **Critical Issue: Cached Image Size Based on Document Size Only**

**Location:** `RemoteSceneController.cpp` lines 297-309 in `RemoteOutlineTextItem::ensureCachedImage()`

```cpp
const QSizeF docSize = layout->documentSize();
// ...
const int targetWidth = std::max(1, static_cast<int>(std::ceil(docSize.width() * targetDeviceScale)));
const int targetHeight = std::max(1, static_cast<int>(std::ceil(docSize.height() * targetDeviceScale)));
```

**Problem:** The cached raster image is sized to match `documentSize()`, which reports the **text content bounds only**, NOT including the stroke overflow. When thick borders are drawn, they extend beyond the document bounds and get clipped by the image edges.

**Host Comparison:** On the host (`TextMediaItem.cpp`), the item's bounding rect and scene geometry already account for `contentPaddingPx()` which includes:
- Base padding (0.0)
- Stroke width
- Overflow allowance: `max(strokeWidth * 0.45, 2.0px)`

This padding is baked into the item's `m_baseSize`, so the host raster has sufficient canvas to draw the full outline.

---

### 2. **Document Margin Not Set to Zero in Cached Renderer**

**Location:** `RemoteSceneController.cpp` line 1789 (text item initialization)

```cpp
if (QTextDocument* doc = textItem->document()) {
    doc->setDocumentMargin(0.0);  // ✓ Set during setup
}
```

But inside `ensureCachedImage()`, the document used for rendering does NOT explicitly set margin to zero. Qt's default document margin (typically 4px) could cause layout shifts between the initial layout (with margin=0) and the cached rendering (with default margin).

**Status:** Margin is set at creation time, so this may not be an active bug, but worth verifying.

---

### 3. **Timing Issue: Cache Built Before Item Visibility**

**Sequence:**
1. Text item created with `setOpacity(0.0)` and implicitly not visible
2. Font, text, outline parameters set
3. Document layout configured (text width, alignment)
4. Item added to scene
5. **Later:** fade-in animation calls `setVisible(true)` and animates opacity

**Issue:** When `setVisible(true)` is called during fade-in (line 2433), the item triggers a paint event. At this moment:
- The cached image is generated for the first time
- The painter's device might be the QGraphicsView's viewport
- Device pixel ratio is read from the painter's device

**With auto-display (no fade):** Items become visible immediately (lines 2415-2419), potentially triggering cache generation during initial scene layout when geometry is stable.

**With fade-in/delay:** Cache generation is deferred until the fade animation starts, at which point the item's geometry/scale might not be fully settled, or the device context differs.

---

### 4. **Missing Padding in Remote Text Layout**

**Location:** `RemoteSceneController.cpp` lines 1889-1906

```cpp
const qreal padding = std::max<qreal>(0.0, strokeWidth + outlineOverflowAllowance(strokeWidth));
// ...
const qreal availableBaseWidth = std::max<qreal>(1.0, baseWidth - 2.0 * padding);
const qreal logicalWidth = std::max<qreal>(1.0, availableBaseWidth / uniformScale);
```

The padding is computed and used to reduce the logical text width (for wrapping), but **the cached image itself is not expanded to include this padding**. The image is still sized to `documentSize()`, which doesn't account for the stroke overflow.

**Correct Approach (Host):**
- Host's `contentPaddingPx()` is added to the base size in scene coordinates
- Text is rendered into a logical rect that is `baseSize - 2*padding`, then scaled
- The item's bounding rect is `baseSize`, providing room for the overflow

**Remote Issue:**
- `baseWidth` and `baseHeight` already include the host's padding
- Text width is set correctly to `(baseWidth - 2*padding) / uniformScale`
- **But the cached image is only `documentSize()` large**, which doesn't include the padding/overflow space

---

### 5. **Scale and Position Timing**

**Location:** `RemoteSceneController.cpp` lines 1910-1942

```cpp
textItem->setScale(appliedScale);
// ...
textItem->setPos(px + horizontalOffset, py + verticalOffset);
```

The item is positioned with padding offsets (`paddingX`, `paddingY`) to center it within the allocated space. However:
- The cached image doesn't extend to fill this padded space
- The image is drawn at (0,0) relative to the item's origin
- The item's bounding rect is determined by the cached image size

**Result:** The item's visual bounds are smaller than the allocated geometry, causing clipping when the thick border overflows the image.

---

### 6. **Device Coordinate Cache Mode**

**Location:** `RemoteSceneController.cpp` line 429

```cpp
setCacheMode(QGraphicsItem::DeviceCoordinateCache);
```

This cache mode stores the item's rendered appearance in device (screen) coordinates, which can improve performance but introduces complexity:
- Cache is invalidated on scale/rotation changes
- May cause rendering differences between initial setup and fade-in

However, this is less likely to cause cropping, more likely to cause quality/performance issues.

---

## Why Auto-Display Works but Fade-In Doesn't

### Auto-Display Path:
1. Scene built, items created at opacity 0.0
2. Auto-display triggers immediately (no delay)
3. Items made visible and opacity set to 1.0 (lines 2415-2419)
4. First paint triggered during stable scene layout
5. Cache generated with correct device context
6. Item remains visible and cached

### Fade-In/Delay Path:
1. Scene built, items created at opacity 0.0
2. Display delayed by timer
3. Scene fully activated, windows shown
4. Timer fires → fade-in animation starts
5. `setVisible(true)` called (line 2433)
6. **First paint triggered during animation**
7. Cache generated while item is mid-transition
8. Potential device context differences or geometry instability

**Key Difference:** The timing of the first paint/cache generation. With auto-display, the cache is built during stable scene setup. With fade-in, it's built during animation start, possibly with different device pixel ratio or before the item's transform is fully applied.

---

## All Possible Issues (Ranked by Likelihood)

### 🔴 **Critical (Definitely causing cropping):**

1. **Cached image sized to documentSize() without stroke overflow**
   - The image is too small to contain the full border
   - Stroke extends beyond image bounds and is clipped
   - **Fix:** Expand image size by `strokeWidth + overflow` on all sides

2. **Missing padding in cached image origin**
   - Text is rendered at (0,0) in the image, leaving no room for left/top overflow
   - **Fix:** Offset the text rendering origin by `padding` within a larger image

### 🟡 **High (Likely contributing):**

3. **Device pixel ratio inconsistency**
   - Cache built with device scale from fade-in context, not initial setup
   - Different scale causes size mismatch between allocated space and cached image
   - **Fix:** Use consistent device scale (e.g., from QGraphicsView) or force recache

4. **Document margin inconsistency**
   - If margin isn't zero during cache render, layout shifts
   - **Fix:** Explicitly set `doc->setDocumentMargin(0.0)` in `ensureCachedImage()`

### 🟢 **Medium (Possible edge cases):**

5. **Bounding rect not overridden**
   - QGraphicsTextItem's default boundingRect() may not account for cached image
   - If boundingRect() returns documentSize() but image is larger, parts may be culled
   - **Fix:** Override `boundingRect()` to return cached image rect

6. **Scale applied before cache built**
   - `setScale()` called before first paint
   - DeviceCoordinateCache mode might cache at wrong scale
   - **Fix:** Switch to ItemCoordinateCache or ensure cache built after scale set

### 🔵 **Low (Unlikely primary cause):**

7. **Padding calculation mismatch**
   - Remote `outlineOverflowAllowance()` lambda might differ from host
   - Both use same formula, so unlikely to be the issue

8. **Text width rounding errors**
   - Logical width calculation could differ slightly from host
   - Would cause wrapping differences, not cropping

9. **Animation-related invalidation**
   - Fade-in opacity animation might trigger cache rebuilds
   - DeviceCoordinateCache doesn't invalidate on opacity change, so unlikely

---

## Recommended Fix Priority

### **Phase 1: Fix Cached Image Size (Critical)**

Expand the cached image to include stroke overflow space, and offset the rendering origin:

```cpp
void ensureCachedImage(QPainter* painter) {
    // ... existing checks ...
    
    const QSizeF docSize = layout->documentSize();
    const qreal strokeWidth = m_strokeWidth;
    
    // Calculate overflow allowance (match host logic)
    qreal overflow = 0.0;
    if (strokeWidth > 0.0) {
        constexpr qreal kOverflowScale = 0.45;
        constexpr qreal kOverflowMinPx = 2.0;
        overflow = std::ceil(std::max<qreal>(strokeWidth * kOverflowScale, kOverflowMinPx));
    }
    const qreal totalPadding = strokeWidth + overflow;
    
    // Expand image size to include overflow on all sides
    const qreal expandedWidth = docSize.width() + 2.0 * totalPadding;
    const qreal expandedHeight = docSize.height() + 2.0 * totalPadding;
    
    const QPaintDevice* paintDevice = painter->device();
    const qreal targetDeviceScale = std::max<qreal>(1.0, paintDevice ? paintDevice->devicePixelRatioF() : 1.0);
    
    // Check if cache is still valid (now including padding)
    if (!m_cacheDirty && 
        qFuzzyCompare(targetDeviceScale, m_cachedDeviceScale) && 
        qFuzzyCompare(expandedWidth, m_cachedExpandedWidth) && 
        qFuzzyCompare(expandedHeight, m_cachedExpandedHeight)) {
        return;
    }
    
    const int targetWidth = std::max(1, static_cast<int>(std::ceil(expandedWidth * targetDeviceScale)));
    const int targetHeight = std::max(1, static_cast<int>(std::ceil(expandedHeight * targetDeviceScale)));
    
    QImage raster(targetWidth, targetHeight, QImage::Format_ARGB32_Premultiplied);
    raster.setDevicePixelRatio(targetDeviceScale);
    raster.fill(Qt::transparent);
    
    QPainter imagePainter(&raster);
    imagePainter.setRenderHint(QPainter::Antialiasing, true);
    imagePainter.setRenderHint(QPainter::TextAntialiasing, true);
    imagePainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    
    // Translate to leave room for overflow on left/top
    imagePainter.translate(totalPadding, totalPadding);
    
    // ... rest of rendering code (highlight, outline, text) ...
    
    imagePainter.end();
    
    m_cachedExpandedWidth = expandedWidth;
    m_cachedExpandedHeight = expandedHeight;
    m_cachedDeviceScale = targetDeviceScale;
    m_cachedImage = std::move(raster);
    m_cacheDirty = false;
}
```

And update paint to draw at offset:

```cpp
void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override {
    Q_UNUSED(option);
    Q_UNUSED(widget);
    if (!painter) return;
    
    ensureCachedImage(painter);
    
    if (!m_cachedImage.isNull()) {
        // Calculate offset to draw image (accounts for overflow padding)
        const qreal overflow = calculateOverflow(); // helper method
        painter->drawImage(QPointF(-overflow, -overflow), m_cachedImage);
    } else {
        QGraphicsTextItem::paint(painter, option, widget);
    }
}
```

### **Phase 2: Override BoundingRect (High Priority)**

Ensure the item's bounding rect includes the full cached image:

```cpp
QRectF boundingRect() const override {
    if (!m_cachedImage.isNull()) {
        const qreal scale = m_cachedImage.devicePixelRatioF();
        const qreal w = m_cachedImage.width() / scale;
        const qreal h = m_cachedImage.height() / scale;
        const qreal overflow = calculateOverflow();
        return QRectF(-overflow, -overflow, w, h);
    }
    return QGraphicsTextItem::boundingRect();
}
```

### **Phase 3: Ensure Document Margin Consistency (Medium Priority)**

Explicitly set document margin in cache renderer:

```cpp
void ensureCachedImage(QPainter* painter) {
    // ... existing checks ...
    
    QTextDocument* doc = document();
    if (doc) {
        doc->setDocumentMargin(0.0); // Ensure consistent with initial setup
    }
    
    // ... rest of method ...
}
```

### **Phase 4: Consider Cache Mode (Low Priority)**

If issues persist, try ItemCoordinateCache:

```cpp
void init() {
    setCacheMode(QGraphicsItem::ItemCoordinateCache); // Instead of DeviceCoordinateCache
    initializeDocumentWatcher();
    markCacheDirty();
}
```

---

## Testing Strategy

1. **Test with thick borders (50-100% width)**
2. **Test fit-to-text enabled**
3. **Test all display modes:**
   - Auto-display (no delay)
   - Auto-display with delay
   - Fade-in (no delay)
   - Fade-in with delay
4. **Test different alignments** (left, center, right / top, center, bottom)
5. **Test different screen scales** (1x, 2x Retina)
6. **Compare host canvas vs remote client** side-by-side

---

## Summary

The primary cause of border cropping is **cached image sized to documentSize() without accounting for stroke overflow**. The image needs to be expanded by `strokeWidth + overflow` on all sides, with the text rendering origin offset accordingly. The fade-in timing difference exposes this bug more reliably because the cache is generated during a different phase of the scene lifecycle, potentially with subtle geometry or device context differences.
