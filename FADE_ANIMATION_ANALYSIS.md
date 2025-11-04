# Analyse: Différence de Vitesse des Fade In/Out Entre Canvas et Client Distant

## Problème Identifié

Les animations de fade-in et fade-out semblent plus rapides ou non linéaires sur le client distant comparé au canvas.

## Analyse Comparative

### Canvas (Host) - MediaItems.cpp lignes 119-163

**Fade In:**
```cpp
void ResizableMediaBase::fadeContentIn(double seconds) {
    // ...
    m_fadeAnimation = new QVariantAnimation();
    m_fadeAnimation->setStartValue(m_contentDisplayOpacity);
    m_fadeAnimation->setEndValue(1.0);
    int durationMs = static_cast<int>(seconds * 1000.0);
    m_fadeAnimation->setDuration(std::max(1, durationMs));
    // ❌ PAS DE setEasingCurve() - utilise Linear par défaut
    m_fadeAnimation->start();
}
```

**Fade Out:**
```cpp
void ResizableMediaBase::fadeContentOut(double seconds) {
    // ...
    m_fadeAnimation = new QVariantAnimation();
    m_fadeAnimation->setStartValue(m_contentDisplayOpacity);
    m_fadeAnimation->setEndValue(0.0);
    int durationMs = static_cast<int>(seconds * 1000.0);
    m_fadeAnimation->setDuration(std::max(1, durationMs));
    // ❌ PAS DE setEasingCurve() - utilise Linear par défaut
    m_fadeAnimation->start();
}
```

**Courbe d'easing:** `QEasingCurve::Linear` (défaut Qt)

---

### Client Distant - RemoteSceneController.cpp

**Fade In (ligne 2343):**
```cpp
auto* anim = new QVariantAnimation(this);
anim->setStartValue(0.0);
anim->setEndValue(item->contentOpacity);
anim->setDuration(durMs);
anim->setEasingCurve(QEasingCurve::OutCubic);  // ⚠️ OutCubic
anim->start();
```

**Fade Out (ligne 2542):**
```cpp
auto* anim = new QVariantAnimation(this);
anim->setStartValue(graphicsItem->opacity());
anim->setEndValue(0.0);
anim->setDuration(durMs);
anim->setEasingCurve(QEasingCurve::OutCubic);  // ⚠️ OutCubic
anim->start();
```

**Courbe d'easing:** `QEasingCurve::OutCubic`

---

## Différence Critique

### `Linear` vs `OutCubic`

**Linear (Canvas):**
- Progression constante: vitesse = 1/durée
- Graphe: ligne droite de 0% à 100%
- Perception: fade régulier, mécanique

**OutCubic (Client Distant):**
- Formule: `y = (x-1)³ + 1`
- Progression rapide au début, décélération à la fin
- 50% de progression atteint à ~20.6% du temps
- 75% de progression atteint à ~37.1% du temps
- 90% de progression atteint à ~53.6% du temps
- Perception: **démarrage très rapide puis ralentissement progressif**

### Impact Perceptuel

Avec `OutCubic`, les 50 premiers % de l'opacité changent en seulement 20% de la durée totale, donnant l'impression que l'animation est:
1. **Plus rapide** - la partie la plus visible (0% → 50%) se produit très vite
2. **Non linéaire** - accélération puis freinage perceptible
3. **Plus "snappy"** - l'effet devient visible quasi-instantanément

Alors que `Linear` distribue le changement uniformément sur toute la durée.

---

## Conclusion

**Le client distant utilise `OutCubic` alors que le canvas utilise `Linear` (défaut).**

C'est pourquoi les fades semblent plus rapides sur le client distant - la majorité du changement d'opacité se produit dans le premier tiers de la durée avec OutCubic, créant une perception de rapidité même si la durée totale est identique.

---

## Solution

Pour avoir le même comportement sur les deux plateformes, il faut:

**Option 1 (Recommandée):** Utiliser `Linear` sur le client distant pour matcher le canvas
**Option 2:** Utiliser `OutCubic` sur le canvas pour matcher le client distant (plus "professionnel")
**Option 3:** Ajouter un paramètre configurable pour la courbe d'easing

La courbe `Linear` est plus prévisible et correspond mieux à ce que l'utilisateur configure (ex: "2 secondes" = vraiment 2 secondes de progression visible), tandis que `OutCubic` donne un résultat plus "poli" mais moins littéral.
