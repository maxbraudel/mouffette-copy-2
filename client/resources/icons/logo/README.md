# Logo Mouffette

`mouffette-logo.svg` est la source unique. Les fichiers générés conservent ses
formes, ses couleurs et sa marge transparente ; ne pas les retoucher séparément.

| Fichiers | Usage |
|---|---|
| `mouffette-*.png` | Icône Qt des fenêtres, du Dock et de la barre des tâches ; tray Windows |
| `mouffette.ico` | Ressource native de l'exécutable Windows, donc aussi de ses raccourcis |
| `mouffette.icns` | Bundle `.app` macOS et icône du volume d'installation DMG |
| `mouffette-tray-macos.png`, `mouffette-tray-macos@2x.png` | Barre de menus macOS, 18 points en résolution normale et Retina |

Les PNG couvrent 16, 20, 24, 32, 40, 48, 64, 128, 256, 512 et 1024 pixels.
L'ICO contient les neuf tailles jusqu'à 256 pixels, notamment pour les facteurs
d'échelle Windows 125 %, 150 % et 200 %. L'ICNS contient les tailles macOS
16, 32, 128, 256 et 512 points en 1× et 2×.

Le tray macOS est un masque : les parties claires du SVG deviennent des découpes
transparentes dans le badge. [`QIcon::setIsMask(true)`](https://doc.qt.io/qt-6/qicon.html#setIsMask) permet à macOS de choisir
la couleur selon la barre de menus et son état de sélection. Un simple disque
opaque perdrait les détails du logo.

## Régénérer après modification du SVG

Sur macOS, installer `librsvg` (`brew install librsvg`) et Pillow dans un
environnement Python, puis lancer depuis la racine du dépôt :

```sh
python3 -m venv /tmp/mouffette-icon-tools
/tmp/mouffette-icon-tools/bin/python -m pip install Pillow
/tmp/mouffette-icon-tools/bin/python client/scripts/generate-icons.py
```

Le script utilise aussi `iconutil`, fourni par macOS. Tous les résultats finaux
sont écrits dans ce dossier. Les fichiers générés sont versionnés : aucune
dépendance de conversion n'est nécessaire pour compiler ou lancer Mouffette.
Si les deux couleurs de référence du SVG changent, adapter aussi leur
correspondance dans `tray_template()`.

## Distribution

Le packaging macOS inclut l'ICNS dans l'application signée et l'emploie comme
`.VolumeIcon.icns` dans le DMG. `SetFile` active l'icône du volume avant sa
compression et sa signature. Le fichier DMG lui-même conserve le type de
document système ; l'icône personnalisée apparaît sur le volume monté.

Windows distribue actuellement une archive ZIP portable, pas un installateur
EXE/MSI. Une archive ZIP n'a pas d'icône d'application personnalisable : c'est
`bin/Mouffette.exe`, à l'intérieur, qui embarque le logo. Un futur installateur
devra réutiliser `mouffette.ico`.

La configuration des ressources natives suit le
[guide Qt des icônes d'application](https://doc.qt.io/qt-6/appicon.html).
