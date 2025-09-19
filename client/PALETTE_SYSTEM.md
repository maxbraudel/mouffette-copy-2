# 🎨 Système de Palette Dynamique - Guide d'utilisation

## Vue d'ensemble

Le nouveau système `ColorSource` vous permet de configurer facilement vos couleurs pour qu'elles s'adaptent automatiquement aux thèmes système ou restent fixes selon vos besoins.

## Configuration des couleurs

### Dans AppColors.cpp, vous pouvez maintenant configurer :

```cpp
// ============================================================================
// CONFIGURATION DES COULEURS - MODIFIEZ CES LIGNES !
// ============================================================================

// Couleur dynamique : s'adapte au thème système
ColorSource gAppBorderColorSource = ColorSource(QPalette::Mid, 70);        // Mid + transparence 70
ColorSource gInteractionBackgroundColorSource = ColorSource(QPalette::Base); // Base du système

// Couleur fixe : ne change jamais
ColorSource gMyCustomColorSource = ColorSource(QColor(255, 0, 0, 128));    // Rouge semi-transparent
```

## Rôles de palette disponibles

### Couleurs de base :
- `QPalette::Window` - Arrière-plan des fenêtres
- `QPalette::WindowText` - Texte sur les fenêtres
- `QPalette::Base` - Arrière-plan des zones d'entrée (blanc/noir selon thème)
- `QPalette::AlternateBase` - Arrière-plan alternatif (gris clair/foncé selon thème)
- `QPalette::Text` - Texte principal (noir/blanc selon thème)
- `QPalette::Button` - Arrière-plan des boutons
- `QPalette::ButtonText` - Texte des boutons

### Couleurs spéciales :
- `QPalette::Highlight` - Couleur de sélection (bleu système)
- `QPalette::HighlightedText` - Texte sélectionné
- `QPalette::Link` - Couleur des liens
- `QPalette::LinkVisited` - Couleur des liens visités
- `QPalette::Mid` - Couleur moyenne (bordures)
- `QPalette::Midlight` - Couleur moyenne claire
- `QPalette::Dark` - Couleur sombre
- `QPalette::Light` - Couleur claire

## Exemples d'usage

### 1. Couleur qui s'adapte au thème
```cpp
// Bordure qui suit la couleur Mid du système avec transparence
ColorSource myBorderSource = ColorSource(QPalette::Mid, 100);
```

### 2. Couleur fixe personnalisée
```cpp
// Rouge vif qui ne change jamais
ColorSource myRedSource = ColorSource(QColor(255, 0, 0));
```

### 3. Utilisation dans le code
```cpp
// Obtenir la couleur actuelle (résout automatiquement les références palette)
QColor currentColor = AppColors::getCurrentColor(myBorderSource);

// Obtenir le CSS (pour les stylesheets)
QString cssColor = AppColors::colorSourceToCss(myBorderSource);

// Utilisation dans un stylesheet
myWidget->setStyleSheet(QString("border: 1px solid %1;").arg(cssColor));
```

## Avantages

✅ **Adaptation automatique** : Les couleurs dynamiques changent instantanément avec le thème système
✅ **Contrôle total** : Vous choisissez quelles couleurs sont dynamiques ou fixes
✅ **Transparence facile** : Spécifiez l'alpha directement dans la configuration
✅ **Compatibilité** : Les anciennes variables `QColor` continuent de fonctionner
✅ **Performance** : Résolution des couleurs à la demande

## Migration

Vos anciennes couleurs continuent de fonctionner ! Pour migrer vers le nouveau système :

1. **Remplacez** `AppColors::colorToCss(AppColors::gMyColor)` 
2. **Par** `AppColors::colorSourceToCss(AppColors::gMyColorSource)`

Cela vous donnera l'adaptation automatique au thème ! 🌓
