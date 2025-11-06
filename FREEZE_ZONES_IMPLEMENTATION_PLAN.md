# 🎨 Plan d'Implémentation : Freeze Zones pour Parties Hors Viewport

## 🎯 Objectif

**Vision :** Au lieu de cacher complètement les parties du texte hors viewport, afficher une version "frozen" (basse résolution) qui ne se met pas à jour lors du zoom, tout en continuant à mettre à jour uniquement la partie visible en haute résolution.

**Exemple concret :**
```
Zoom out (100%) → Tout le texte visible, basse résolution (0.5 MP)
  ↓
Zoom in (800%) → Partie visible haute résolution (0.52 MP)
                 Parties hors viewport gardent la résolution 100% (frozen)
```

---

## 🔍 Analyse Approfondie du Système Actuel

### 1. **État de l'Art : Système Actuel**

**Architecture de rasterisation actuelle :**

```
┌─────────────────────────────────────────────────────────┐
│ SYSTÈME ACTUEL (Post Option A)                         │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  paint() appelé                                         │
│    ↓                                                    │
│  ensureScaledRaster(effectiveScale, geometryScale, zoom)│
│    ↓                                                    │
│  computeVisibleRegion() → QRectF visibleRegion         │
│    ↓                                                    │
│  TextRasterJob { targetRect = visibleRegion }          │
│    ↓                                                    │
│  execute() → QImage de SEULEMENT visibleRegion         │
│    ↓                                                    │
│  m_scaledRasterPixmap = pixmap(visibleRegion)          │
│  m_scaledRasterVisibleRegion = visibleRegion           │
│    ↓                                                    │
│  paint() → drawPixmap(scaledVisibleRegion, pixmap)     │
│                                                         │
│  ❌ PROBLÈME : Parties hors viewport = RIEN            │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

**Variables clés (TextMediaItem.h lignes 223-229) :**
```cpp
QPixmap m_scaledRasterPixmap;              // Cache actuel (viewport only)
bool m_scaledRasterPixmapValid = false;
QRectF m_scaledRasterVisibleRegion;        // Région rasterisée
QRectF m_lastViewportRect;                 // Dernier viewport
qreal m_lastViewportScale = 1.0;           // Dernier zoom
```

**Comportement actuel dans paint() (lignes 2610-2632) :**
```cpp
if (m_scaledRasterPixmapValid && !m_scaledRasterPixmap.isNull()) {
    QRectF destRect = scaledBounds;
    if (!m_scaledRasterVisibleRegion.isEmpty()) {
        const QRectF scaledVisibleRegion = scaleTransform.mapRect(m_scaledRasterVisibleRegion);
        destRect = scaledVisibleRegion;  // ✅ Dessine SEULEMENT viewport
    }
    painter->drawPixmap(destRect, m_scaledRasterPixmap, sourceRect);
}
// ❌ Parties hors viewport : RIEN n'est dessiné
```

---

### 2. **Problème Actuel Visualisé**

**Scénario :**
```
Canvas zoomed out (100%) :
┌─────────────────────────────────┐
│ Lorem ipsum dolor sit amet,     │  ← Tout visible, 1 MP
│ consectetur adipiscing elit.    │     Résolution : 100%
│ Sed do eiusmod tempor.          │
└─────────────────────────────────┘

Zoom in (800%) sur "Lorem" :
┌────────────┬────────────────────┐
│ LOREM      │                    │  ← Viewport visible, 0.52 MP
│ IPSUM      │                    │     Résolution : 800%
├────────────┴────────────────────┤
│ ❌ VIDE (caché)                 │  ← Hors viewport
│ consectetur adipiscing elit.    │     ❌ Rien n'est affiché !
│ Sed do eiusmod tempor.          │
└─────────────────────────────────┘
```

**Comportement souhaité :**
```
Zoom in (800%) sur "Lorem" :
┌────────────┬────────────────────┐
│ LOREM      │                    │  ← Viewport, 0.52 MP
│ IPSUM      │                    │     Résolution : 800%
├────────────┴────────────────────┤
│ consectetur adipiscing elit.    │  ← Freeze zone, 0.3 MP
│ Sed do eiusmod tempor.          │     Résolution : 100% (frozen)
│ (flou mais visible)             │     ✅ Pas de mise à jour !
└─────────────────────────────────┘
```

---

## 🏗️ Architecture Proposée : Système Multi-Cache

### Concept : Deux Niveaux de Cache

```
┌──────────────────────────────────────────────────────────┐
│ NOUVEAU SYSTÈME : DUAL-CACHE                             │
├──────────────────────────────────────────────────────────┤
│                                                          │
│  1. HIGH-RES CACHE (Viewport visible)                   │
│     ✅ Mis à jour constamment lors du zoom/pan          │
│     ✅ Haute résolution (800%)                          │
│     ✅ Partial rendering (0.52 MP)                      │
│     → m_scaledRasterPixmap (actuel)                     │
│     → m_scaledRasterVisibleRegion                       │
│                                                          │
│  2. LOW-RES FALLBACK CACHE (Texte complet)              │
│     ✅ Rasterisé une fois à zoom out                    │
│     ✅ Basse résolution (100-200%)                      │
│     ✅ Full text rendering (1-2 MP)                     │
│     → m_frozenFallbackPixmap (NOUVEAU)                  │
│     → m_frozenFallbackScale (NOUVEAU)                   │
│     → m_frozenFallbackValid (NOUVEAU)                   │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

---

## 📐 Plan d'Implémentation Détaillé

### **Phase 1 : Ajout du Fallback Cache (2h)**

#### Étape 1.1 : Nouvelles variables membres (TextMediaItem.h)

**Après ligne 229 :**
```cpp
// Freeze zone fallback cache - low-res full text for out-of-viewport regions
QPixmap m_frozenFallbackPixmap;           // Full text at low res
bool m_frozenFallbackValid = false;       // Is fallback cache valid
qreal m_frozenFallbackScale = 1.0;        // Scale at which fallback was created
QSize m_frozenFallbackSize;               // Size of text when fallback created
```

**Pourquoi ces variables :**
- `m_frozenFallbackPixmap` : Cache basse résolution du texte COMPLET
- `m_frozenFallbackScale` : Zoom auquel le fallback a été créé (ex: 1.5× ou 2×)
- `m_frozenFallbackValid` : Invalider si texte modifié
- `m_frozenFallbackSize` : Détecter si dimensions du texte ont changé

#### Étape 1.2 : Création du fallback cache

**Nouvelle méthode dans TextMediaItem.cpp :**
```cpp
void TextMediaItem::ensureFrozenFallbackCache(qreal currentCanvasZoom) {
    // Strategy: Create fallback at 1.5-2× resolution when zoomed in
    // This provides enough quality for periphery while staying lightweight
    
    const qreal epsilon = 1e-4;
    const qreal targetFallbackScale = 1.5;  // Fixed low-res for fallback
    
    // Check if we need to create/update fallback
    const bool textChanged = m_baseSize != m_frozenFallbackSize;
    const bool scaleChanged = std::abs(m_frozenFallbackScale - targetFallbackScale) > epsilon;
    const bool needsUpdate = !m_frozenFallbackValid || textChanged || scaleChanged;
    
    // Only create fallback when zoomed significantly (> 300%)
    // Below that, the high-res cache covers most of the screen anyway
    if (currentCanvasZoom < 3.0) {
        return;
    }
    
    if (!needsUpdate) {
        return;
    }
    
    // Calculate size for fallback (full text at low res)
    const int fallbackWidth = std::max(1, static_cast<int>(
        std::ceil(m_baseSize.width() * targetFallbackScale)
    ));
    const int fallbackHeight = std::max(1, static_cast<int>(
        std::ceil(m_baseSize.height() * targetFallbackScale)
    ));
    const QSize fallbackSize(fallbackWidth, fallbackHeight);
    
    // Render full text at low resolution (no viewport clipping)
    QImage fallbackImage;
    renderTextToImage(fallbackImage, fallbackSize, targetFallbackScale, QRectF());  // Empty rect = full render
    
    m_frozenFallbackPixmap = QPixmap::fromImage(fallbackImage);
    m_frozenFallbackPixmap.setDevicePixelRatio(1.0);
    m_frozenFallbackValid = !m_frozenFallbackPixmap.isNull();
    m_frozenFallbackScale = targetFallbackScale;
    m_frozenFallbackSize = m_baseSize;
}
```

**Logique :**
1. Créer fallback seulement si zoom > 300% (sinon inutile)
2. Résolution fixe 1.5× (compromis qualité/mémoire)
3. Rasterise le texte COMPLET (pas de clipping viewport)
4. Invalider si texte modifié ou dimensions changées

---

### **Phase 2 : Modification du Paint() pour Dual-Rendering (1.5h)**

#### Étape 2.1 : Nouvelle logique de peinture

**Modifier paint() (lignes 2598-2650) :**

```cpp
if (needsScaledRaster) {
    ensureScaledRaster(effectiveScale, currentScale, canvasZoom);
    ensureFrozenFallbackCache(canvasZoom);  // ✅ Create/update fallback
    
    painter->save();
    const qreal epsilon = 1e-4;
    const qreal totalScale = std::max(std::abs(currentScale * uniformScale * canvasZoom), epsilon);
    painter->scale(1.0 / totalScale, 1.0 / totalScale);
    
    const QTransform scaleTransform = QTransform::fromScale(totalScale, totalScale);
    QRectF scaledBounds = scaleTransform.mapRect(bounds);
    
    // ✅ STEP 1: Draw frozen fallback for ENTIRE text (low-res background)
    if (m_frozenFallbackValid && !m_frozenFallbackPixmap.isNull() && canvasZoom > 3.0) {
        const qreal fallbackDisplayScale = totalScale / m_frozenFallbackScale;
        
        painter->save();
        painter->scale(fallbackDisplayScale, fallbackDisplayScale);
        
        const QRectF fallbackDestRect(
            QPointF(0, 0),
            QSizeF(m_frozenFallbackPixmap.width(), m_frozenFallbackPixmap.height())
        );
        const QRectF fallbackSourceRect(
            QPointF(0, 0),
            QSizeF(m_frozenFallbackPixmap.width(), m_frozenFallbackPixmap.height())
        );
        
        painter->drawPixmap(fallbackDestRect, m_frozenFallbackPixmap, fallbackSourceRect);
        painter->restore();
    }
    
    // ✅ STEP 2: Draw high-res viewport cache ON TOP (overwrites visible region)
    if (m_scaledRasterPixmapValid && !m_scaledRasterPixmap.isNull()) {
        const QSizeF sourceSize(
            static_cast<qreal>(m_scaledRasterPixmap.width()),
            static_cast<qreal>(m_scaledRasterPixmap.height())
        );
        const QRectF sourceRect(QPointF(0.0, 0.0), sourceSize);
        
        QRectF destRect = scaledBounds;
        if (!m_scaledRasterVisibleRegion.isEmpty() && m_scaledRasterVisibleRegion.isValid()) {
            const QRectF scaledVisibleRegion = scaleTransform.mapRect(m_scaledRasterVisibleRegion);
            destRect = scaledVisibleRegion;
        }
        
        painter->drawPixmap(destRect, m_scaledRasterPixmap, sourceRect);
    }
    
    painter->restore();
}
```

**Logique de rendu en deux passes :**
1. **Passe 1 (Fond)** : Dessiner fallback basse-res pour TOUT le texte
2. **Passe 2 (Premier plan)** : Dessiner cache haute-res UNIQUEMENT sur viewport visible
   - Résultat : Viewport = net, périphérie = flou mais visible

---

### **Phase 3 : Invalidation Intelligente du Fallback (1h)**

#### Étape 3.1 : Invalider fallback lors de modifications texte

**Modifier les fonctions qui changent le texte :**

```cpp
void TextMediaItem::setText(const QString& text) {
    if (m_text == text) return;
    m_text = text;
    m_needsRasterization = true;
    m_scaledRasterDirty = true;
    m_frozenFallbackValid = false;  // ✅ Invalider fallback
    update();
}

void TextMediaItem::setFont(...) {
    // ... existing code ...
    m_frozenFallbackValid = false;  // ✅ Invalider fallback
}

// Idem pour setTextColor, setBorderWidth, etc.
```

#### Étape 3.2 : Stratégie de mise à jour fallback

**Quand créer/mettre à jour le fallback :**

| Condition | Action | Raison |
|-----------|--------|--------|
| Zoom < 300% | Pas de fallback | Cache haute-res couvre tout l'écran |
| Zoom > 300% && !fallback | Créer fallback 1.5× | Périphérie devient invisible |
| Texte modifié | Invalider + recréer | Contenu changé |
| Zoom change | Garder fallback | Pas besoin de refaire, juste rescaler |
| Pan viewport | Garder fallback | Périphérie reste frozen |

---

## 🎨 Optimisations Avancées (Phase 4 - Optionnel, 3h)

### Option 4.1 : Fallback Scale Adaptatif

**Problème :** Fallback fixe à 1.5× peut être trop flou si zoom extrême (2000%)

**Solution :** Adapter la résolution du fallback au zoom actuel

```cpp
void TextMediaItem::ensureFrozenFallbackCache(qreal currentCanvasZoom) {
    // Adaptive fallback scale based on zoom level
    qreal targetFallbackScale = 1.5;
    
    if (currentCanvasZoom > 10.0) {
        targetFallbackScale = 3.0;      // Zoom extrême → fallback 3×
    } else if (currentCanvasZoom > 5.0) {
        targetFallbackScale = 2.0;      // Zoom élevé → fallback 2×
    } else {
        targetFallbackScale = 1.5;      // Zoom normal → fallback 1.5×
    }
    
    // ... reste du code
}
```

**Avantages :**
- Meilleure qualité périphérie à zoom élevé
- Toujours moins de pixels que cache haute-res viewport

**Inconvénients :**
- Plus de mémoire à zoom extrême
- Re-rasterisation si seuil de zoom franchi

---

### Option 4.2 : Transition Douce (Fade In/Out)

**Problème :** Bord abrupt entre zone haute-res et zone basse-res

**Solution :** Gradient de transition sur 10-20px

```cpp
// Dans paint(), après avoir dessiné les deux caches
if (m_frozenFallbackValid && m_scaledRasterPixmapValid) {
    // Create soft edge mask for high-res cache
    const qreal featherWidth = 20.0;  // Pixels de transition
    
    // Apply gradient mask at edges of visible region
    // ... code QPainterPath avec gradient radial
}
```

**Avantages :**
- Transition visuelle plus douce
- Moins de "saut" perceptible

**Inconvénients :**
- Complexité accrue
- Performance impact (masque alpha)

---

### Option 4.3 : Lazy Fallback Creation

**Problème :** Créer fallback immédiatement peut bloquer UI

**Solution :** Créer fallback de façon async après première frame

```cpp
void TextMediaItem::ensureFrozenFallbackCache(qreal currentCanvasZoom) {
    if (currentCanvasZoom < 3.0) return;
    
    if (!m_frozenFallbackValid && !m_fallbackJobInProgress) {
        // Launch async job to create fallback
        m_fallbackJobInProgress = true;
        
        auto future = QtConcurrent::run([this, targetFallbackScale]() {
            QImage fallbackImage;
            renderTextToImage(fallbackImage, fallbackSize, targetFallbackScale, QRectF());
            return fallbackImage;
        });
        
        // Handle completion...
    }
}
```

**Avantages :**
- Pas de blocage UI
- Fallback apparaît progressivement

**Inconvénients :**
- Complexité async (race conditions)
- Périphérie vide pendant 1ère frame

---

## 📊 Analyse Mémoire et Performance

### Consommation Mémoire

**Avant (système actuel) :**
```
Zoom 800%, texte 400×200px, viewport 960×540px :
- High-res cache : 960×540×4 = 2.07 MB
- Total : 2.07 MB
```

**Après (système dual-cache) :**
```
Zoom 800%, texte 400×200px, viewport 960×540px :
- High-res cache (viewport) : 960×540×4 = 2.07 MB
- Fallback cache (full text 1.5×) : 600×300×4 = 0.72 MB
- Total : 2.79 MB (+35%)
```

**Zoom 2000% :**
```
Avant :
- High-res cache : 400×200×4 = 0.32 MB (viewport tiny)
- Total : 0.32 MB

Après :
- High-res cache : 400×200×4 = 0.32 MB
- Fallback cache : 600×300×4 = 0.72 MB
- Total : 1.04 MB (+225% mais toujours < 2 MB)
```

**Conclusion mémoire :**
- Augmentation modérée (< 1 MB par texte)
- Acceptable pour expérience visuelle améliorée

---

### Performance

**Création du fallback :**
- Résolution 1.5× : ~10-15ms (sync)
- Alternative async : 0ms blocage, création en background

**Rendering dans paint() :**
- Avant : 1 drawPixmap() → ~0.5ms
- Après : 2 drawPixmap() → ~1ms (+100% mais négligeable)

**Cache hit rate :**
- Fallback créé 1× puis réutilisé
- Pas de re-création lors du pan (frozen)
- Re-création seulement si texte modifié

---

## 🎯 Recommandation : Plan d'Action Minimal

### **Approche Recommandée : Phases 1-3 (4.5h total)**

**Phase 1 : Dual-Cache Foundation (2h)**
1. Ajouter variables `m_frozenFallback*` dans header
2. Implémenter `ensureFrozenFallbackCache()`
3. Créer fallback à résolution fixe 1.5×
4. Seuil activation : zoom > 300%

**Phase 2 : Dual-Rendering (1.5h)**
1. Modifier `paint()` pour deux passes
2. Passe 1 : Fallback full text (fond)
3. Passe 2 : High-res viewport (premier plan)
4. Tester visually

**Phase 3 : Invalidation (1h)**
1. Invalider fallback lors setText/setFont/etc.
2. Conserver fallback lors du pan/zoom
3. Logger création fallback pour debug

**Tests de validation :**
1. Zoom out 100% → pas de fallback
2. Zoom in 500% → fallback créé
3. Pan viewport → périphérie visible en basse-res
4. Éditer texte → fallback invalidé + recréé
5. Mesurer mémoire : < 3 MB par texte

---

### **Phase 4 Optionnelle (si nécessaire) :**

**Implémenter seulement si :**
- Utilisateurs se plaignent de qualité fallback → Option 4.1 (Adaptive Scale)
- Bord visible entre zones → Option 4.2 (Fade Transition)
- Lag lors création fallback → Option 4.3 (Async Creation)

---

## 🏁 Résumé Exécutif

### Problème
Actuellement, les parties du texte hors viewport sont **totalement cachées**, créant une expérience visuelle désorientante lors du zoom.

### Solution
Système **dual-cache** : 
- Cache haute-res (viewport visible, mis à jour constamment)
- Cache basse-res (texte complet, frozen lors du zoom)

### Bénéfices
✅ Périphérie visible même zoomé (contexte spatial)  
✅ Pas de mise à jour périphérie = performance préservée  
✅ Coût mémoire modéré (+0.7 MB par texte)  
✅ Implémentation simple (4.5h)  

### Risques
⚠️ +35% mémoire (acceptable)  
⚠️ Double rendering dans paint() (+0.5ms négligeable)  
⚠️ Complexité code modérée  

### Go/No-Go
**✅ GO** - Excellent ratio bénéfice/coût, amélioration UX significative

---

## 📝 Fichiers à Modifier

### `TextMediaItem.h`
- Ajouter membres `m_frozenFallback*` après ligne 229

### `TextMediaItem.cpp`
- Nouvelle méthode `ensureFrozenFallbackCache()` (~50 lignes)
- Modifier `paint()` dual-rendering (~30 lignes)
- Invalider fallback dans `setText()`, `setFont()`, etc. (~10 endroits, 1 ligne chacun)

**Total estimation : ~150 lignes de code ajoutées/modifiées**

---

**Prêt à implémenter Phase 1-3 ? 🚀**
