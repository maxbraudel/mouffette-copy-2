# Commandes de build et de lancement

Tous les scripts publics sont lancés depuis la racine `client`. Les fichiers
`.sh` ciblent macOS et les fichiers `.ps1` ciblent Windows. Le dossier
`internal` contient l'implémentation partagée et ne constitue pas une interface
publique.

## Matrice des commandes

| Action | macOS | Windows PowerShell | Sortie |
|---|---|---|---|
| Compiler Development | `./scripts/build-development.sh` | `.\scripts\build-development.ps1` | `out/build/macos-debug` ou `out/build/windows-debug` |
| Lancer Development | `./scripts/run-development.sh` | `.\scripts\run-development.ps1` | Build locale Development |
| Compiler Release | `./scripts/build-release.sh` | `.\scripts\build-release.ps1` | `out/build/macos-release` ou `out/build/windows-release` |
| Lancer la Release locale | `./scripts/run-release.sh` | `.\scripts\run-release.ps1` | Build locale, avec les dépendances du poste |
| Créer le package Release | `./scripts/package-release.sh` | `.\scripts\package-release.ps1` | `out/stage/*-release`, puis `out/packages` |
| Lancer la Release packagée | `./scripts/run-packaged-release.sh` | `.\scripts\run-packaged-release.ps1` | Stage autonome, sans injecter Qt/MSYS2 |

Development est une build Debug du canal `development` (`Mouffette`). Une
Release locale et une Release packagée appartiennent toutes deux au canal
`production` (`Mouffette`) et reçoivent donc exactement les mêmes overrides de
production. Les canaux ont des verrous IPC distincts, mais le profil principal
conserve les chemins et QSettings historiques. Le package n'est pas une autre configuration de compilation : il
installe la Release dans un stage propre, déploie Qt, signe, archive et calcule
un SHA-256.

## Options

- Les deux scripts de build acceptent `--clean` et `--target <cible>` sur
  macOS, ou `-Clean` et `-Target <cible>` sur Windows. `-ConsoleLogs` est aussi
  disponible sur Windows.
- Sur Windows, les builds utilisent directement `ucrt64\bin\cmake.exe` sous
  `MOUFFETTE_MSYS2_ROOT` (par défaut `C:\msys64`), y compris pour le contrôle
  d'architecture. Si CMake manque, installer `mingw-w64-ucrt-x86_64-cmake`
  depuis le terminal MSYS2 UCRT64 avec
  `pacman -S --needed mingw-w64-ucrt-x86_64-cmake`.
- `package-release` exécute explicitement `build-release`, puis tous les tests.
  `--skip-tests`/`-SkipTests` est réservé au diagnostic local.
- Sur macOS, `--require-signing` impose
  `MOUFFETTE_MACOS_SIGN_IDENTITY`; la notarisation utilise
  `MOUFFETTE_NOTARY_PROFILE` et peut être désactivée avec `--no-notarize`.
- Sur Windows, `-RequireSigning` impose
  `MOUFFETTE_WINDOWS_CERTIFICATE`; son mot de passe vient de
  `MOUFFETTE_WINDOWS_CERTIFICATE_PASSWORD`.
- `MOUFFETTE_KEEP_PACKAGE_WORK_DIR=1` conserve le répertoire de travail macOS
  pour diagnostiquer un déploiement.

## Configuration et multi-instance

La priorité de configuration est fixe :

```text
défauts compilés < .env < réglages du profil < environnement processus < CLI
< clés présentes dans .env.production (Release uniquement)
```

Une clé absente de `.env.production` ne modifie pas la valeur commune. En
Development, `.env` active `MOUFFETTE_ALLOW_MULTIPLE_INSTANCES=true`. Chaque
lancement prend le plus petit numéro libre ; les profils 2 et suivants ont un
stockage temporaire privé, supprimé à leur fermeture ou récupéré après un
crash. La clé d'installation reste commune, tandis que chaque profil obtient
un endpoint distinct.

```bash
./scripts/run-development.sh &
./scripts/run-development.sh &
./scripts/run-development.sh &
```

En Release, `.env.production` force
`MOUFFETTE_ALLOW_MULTIPLE_INSTANCES=false` en dernier. Ni QSettings, ni une
variable de processus, ni `--allow-multiple-instances=true` ne peuvent le
réactiver. Un second lancement demande à l'instance existante de restaurer et
d'activer sa fenêtre, puis se termine avec succès avant toute connexion réseau.

## Sorties et nettoyage

```text
out/build/<plateforme>-<configuration>  objets et application locale
out/stage/<plateforme>-release          application autonome recréée à neuf
out/packages                            DMG/ZIP versionné et fichier .sha256
```

`--clean`/`-Clean` reconfigure le répertoire CMake depuis zéro. Le packaging
supprime toujours son stage final avant installation et utilise un répertoire
de travail temporaire pour ne jamais déployer Qt dans le dossier du compilateur.
