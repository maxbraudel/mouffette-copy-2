**Audit de l’import vidéo, des thumbnails et du scrubbing — Mouffette**

20 septembre 2026. Analyse du code présent sur disque, y compris les modifications non commitées, recherches dans les sources officielles et essais locaux ciblés. Aucun code applicatif modifié. Les comportements visuels rapportés par l’utilisateur n’ont pas fait l’objet d’une capture comparative instrumentée ; les mécanismes ci-dessous sont établis par lecture du code. Les chiffres locaux sont identifiés séparément.

**Diagnostic**

Trois mécanismes se cumulent : l’aperçu attend une validation intégrale du média ; le zoom invalide les images de la bande avant que leurs remplaçantes soient disponibles ; le scrubbing demande des images exactes sur la vidéo originale à longues dépendances temporelles. Mettre le fichier compressé en RAM retire les lectures disque du chemin critique, mais ne rend pas chaque image indépendamment décodable.

Qt Quick et le rendu actuel offrent déjà de bonnes bases : travail en arrière-plan, mutualisation des assets, pool borné, partage des frames et des textures, rendu natif YUV, virtualisation des clips visibles. Les corrections prioritaires concernent la politique de préparation, de conservation et d’ordonnancement.

**Ce qui a été mesuré dans cette session**

| Observation | Résultat | Portée |
|---|---|---|
| Fichier fourni | H.264, 1080 × 1920, environ 71,47 s, 2 111 images | Inspection ffprobe de VID_20260920_013247.mp4 |
| Images clés | 10 ; intervalle médian environ 8,33 s, maximum 8,6 s | Caractéristiques de ce fichier, pas de toutes les vidéos |
| Import actuel | État ready à 6 096 ms ; analyse/entrée décodage vers 129 ms, progression 90 % à 5 869 ms | Un essai natif Debug, Qt 6.11.2, macOS 26.1 ; chemin MediaResidencyManager avec original conservé |
| Régression import/préparation | PASS ; durée totale du test 6 917 ms | Vérifie aussi début, positions intermédiaires et fin ; ne mesure pas leur latence séparément |
| Régressions ResidentMedia ciblées | 7 résultats PASS, aucun échec, 626 ms | Total incluant init/cleanup ; couverture notamment B-frames/VFR et original conservé |

Les exécutables utilisés sont postérieurs aux sources média et au test d’import ; le cache CMake confirme Debug. Il ne s’agit ni d’une campagne Release, ni d’une mesure de latence souris → écran. Les caches du système n’ont pas été purgés : ne pas qualifier cette mesure d’import « à froid ».

La documentation [MEDIA_ENGINE_VALIDATION.md](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/docs/MEDIA_ENGINE_VALIDATION.md>) contient des chiffres intéressants, mais ses tableaux historiques de scrub concernent une représentation all-intra. Le document le signale lui-même. L’import interactif actuel conserve l’original : ces tableaux ne qualifient donc pas son comportement sur ce H.264.

Pour reproduire l’essai d’import depuis la racine du projet :

```sh
MOUFFETTE_IMPORT_TEST_FILE="$PWD/VID_20260920_013247.mp4" \
  client/out/build/macos-debug/tst_MediaResidencyManager suppliedSourceFinishesPreparation
```

**1. L’import attend beaucoup plus qu’une copie en RAM**

Le chemin actuel est : probe et premier SHA-256 complet → nouvelle lecture complète en RAM et nouveau SHA-256 → ouverture FFmpeg → décodage de validation de toutes les images et de tout l’audio sélectionné jusqu’à la fin → indexage complémentaire des paquets → préparation du lecteur → publication ready. Il ne transcode plus systématiquement toute la vidéo à l’import : retainOriginalVideo est explicitement activé.

La validation complète est sérialisée pour limiter la mémoire temporaire : un second média peut donc attendre le premier. Le double parcours pour calculer le hash est une autre optimisation possible, à condition de conserver la vérification d’identité et la détection des fichiers modifiés. Voir [MediaResidencyManager.cpp:477](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/MediaResidencyManager.cpp:477>) et [ordonnancement des imports:539](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/MediaResidencyManager.cpp:539>).

Les points d’entrée sont [MediaDecoder.cpp:585](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/MediaDecoder.cpp:585>), [MediaDecoder.cpp:693](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/MediaDecoder.cpp:693>), [IndexedMediaDecoder.cpp:111](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/IndexedMediaDecoder.cpp:111>) et [MediaResidencyManager.cpp:704](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/MediaResidencyManager.cpp:704>).

Le gestionnaire ne délivre l’asset aux thumbnails qu’une fois ready. La bande lance ensuite ses demandes de miniatures : l’utilisateur subit donc une attente supplémentaire après le chargement. La première frame est pourtant déjà produite au cours du décodage. Voir [MediaResidencyManager.cpp:412](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/MediaResidencyManager.cpp:412>) et [TimelineThumbnailItem.cpp:65](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/frontend/rendering/canvas/TimelineThumbnailItem.cpp:65>).

Recommandation : séparer les états « métadonnées disponibles », « aperçu disponible », « validation complète », « prêt pour exécution de scène ». Un poster et quelques vignettes peuvent être présentés progressivement, tandis que la validation complète continue et reste une condition pour les usages qui l’exigent, notamment le lancement d’une scène distante.

Cela suppose un canal d’aperçu distinct ou des snapshots immuables : publier un asset partiellement construit pendant qu’un autre thread modifie ses index introduirait des courses. L’identité provisoire d’aperçu doit être invalidée si le fichier change, puis rattachée au hash définitif. Cette proposition modifie le contrat d’affichage, sans nécessiter de diminuer les garanties de validation finale.

**2. Le clignotement au zoom est directement expliqué par le code**

La largeur des tuiles reste en pixels, mais leur image est choisie par :

```text
temps source = sourceIn + position de la tuile / pixelsParMilliseconde
```

Une variation de zoom peut donc changer une grande partie des images demandées. Le code annule les demandes de la génération précédente, supprime les samples devenus inutiles, puis ne dessine rien aux emplacements dont la nouvelle image manque. Il supprime également les anciens quads et textures non réutilisés. Voir [TimelineThumbnailItem.cpp:117](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/frontend/rendering/canvas/TimelineThumbnailItem.cpp:117>), [suppression des quads/textures:157](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/frontend/rendering/canvas/TimelineThumbnailItem.cpp:157>) et [renouvellement des demandes:174](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/frontend/rendering/canvas/TimelineThumbnailItem.cpp:174>).

Cette politique produit nécessairement des trous lorsque les nouvelles images ne sont pas immédiatement disponibles. La durée et la fréquence de ces trous restent à mesurer. Les changements d’image peuvent aussi donner l’impression que la bande glisse : cela ne prouve pas une erreur de coordonnées dans le GPU.

La géométrie existante conserve correctement l’ancrage des tuiles lors d’un scroll à zoom fixe et découpe les images aux bords sans les comprimer. Il faut préserver ces comportements.

Première correction : garder une image déjà affichée jusqu’à disponibilité de son remplacement, avec une miniature proche dans le temps ou le poster du même média comme solution provisoire. Les textures existantes doivent rester utilisables pendant ce délai.

Qt fournit précisément ce comportement pour ses éléments Image avec retainWhileLoading depuis Qt 6.8. Mouffette dessine ses thumbnails avec des nœuds C++ : cette propriété ne peut pas être ajoutée telle quelle ; il faut implémenter son équivalent dans TimelineThumbnailItem. [Documentation Qt Image](https://doc.qt.io/qt-6/qml-qtquick-image.html#retainWhileLoading-prop).

**3. Le cache et les priorités aggravent les trous**

Les samples CPU sont détenus par références faibles. Leur éviction du cache LRU peut rendre une tuile vide même si sa texture GPU existe encore. Le rendu vérifie d’abord la présence du sample CPU et abandonne ensuite la texture. Le mécanisme de récupération relance le travail ; il n’assure pas la continuité visuelle. Voir [TimelineThumbnailItem.h:45](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/frontend/rendering/canvas/TimelineThumbnailItem.h:45>) et [TimelineThumbnailItem.cpp:124](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/frontend/rendering/canvas/TimelineThumbnailItem.cpp:124>).

La bande demande la zone visible et une largeur de viewport de chaque côté, puis trie toutes les demandes par temps source. Une série hors écran à gauche peut ainsi occuper les workers avant les images attendues à l’écran. Tout est classé Thumbnail. L’ordre temporel aide le décodage, mais il doit intervenir après la priorité de visibilité.

Chaque miniature absente déclenche actuellement un décodage pleine définition, une conversion en QImage, puis une réduction jusqu’à 192 × 108. Cette voie ne consulte pas d’abord le poster ou le cache de frames vidéo déjà décodées. Voir [DecodeScheduler.cpp:125](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/DecodeScheduler.cpp:125>) et [DecodeScheduler.cpp:148](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/DecodeScheduler.cpp:148>).

Corrections proposées :

- Conserver des références fortes, comptabilisées et bornées, pour les ressources réellement affichées ; garder le LRU pour le voisinage et l’historique.
- Prioriser visible → voisinage dans le sens du déplacement → reste ; regrouper ensuite par média et proximité temporelle.
- Conserver les demandes encore utiles entre deux layouts ; annuler seulement la différence.
- Produire le premier thumbnail depuis la première frame existante ; réutiliser les frames vidéo disponibles lorsque possible.
- Pour les nouvelles extractions, convertir/redimensionner vers la petite taille directement quand le chemin FFmpeg le permet. Cela économise la conversion RGB pleine taille ; cela ne supprime pas le décodage des références du codec.
- Ajouter un cache disque borné des miniatures, distinct du budget de frames actives. Le cache doit inclure identité du contenu, flux, timestamp/frame, taille, transformations et version de génération.

Le cache CPU actuel de 32 Mio et le cache de frames de 64 Mio sont des budgets facultatifs, pas des plafonds de RAM totale ou de textures GPU.

**4. Le scrubbing a un coût très différent de Play**

Lors d’un déplacement arrière ou d’un saut distant, IndexedMediaDecoder effectue un seek vers une image clé, vide l’état du codec, puis décode jusqu’à l’image cible. Sur le fichier fourni, un intervalle de 8,6 s représente de l’ordre de 250 images à parcourir à environ 30 images/s. Un playback séquentiel profite au contraire de l’historique du décodeur. Voir [IndexedMediaDecoder.cpp:381](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/IndexedMediaDecoder.cpp:381>).

FFmpeg documente que av_seek_frame se positionne sur une image clé. Le fichier en RAM accélère l’accès aux paquets, mais ne change pas les dépendances entre images. [API FFmpeg de démultiplexage et seek](https://www.ffmpeg.org/doxygen/trunk/group__lavf__decoding.html).

Le décodeur actuel est logiciel avec thread_count = 1 par contexte. Le pool contient au maximum quatre workers. Chaque worker garde une seule session de vidéo originale : passer d’un média à un autre remplace cette session. Il n’existe pas d’affinité explicite média/worker. Les thumbnails et le scrubbing se partagent ces mêmes workers. Voir [IndexedMediaDecoder.cpp:28](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/IndexedMediaDecoder.cpp:28>), [sessions originales:330](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/IndexedMediaDecoder.cpp:330>) et [DecodeScheduler.cpp:53](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/DecodeScheduler.cpp:53>).

Le logiciel utilise déjà le GPU pour dessiner les plans vidéo ; cela ne signifie pas que le décodage H.264 est matériel.

**5. La dernière position demandée n’est pas encore une vraie politique de présentation**

Pendant le drag, ResidentVideoPlayer conserve une seule demande en vol. Les déplacements suivants modifient la cible, mais laissent terminer l’ancien décodage. Son résultat est présenté avant de redemander la position la plus récente. Voir [ResidentVideoPlayer.cpp:161](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/ResidentVideoPlayer.cpp:161>) et [ResidentVideoPlayer.cpp:280](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/ResidentVideoPlayer.cpp:280>).

La file ne croît donc pas sans limite, ce qui est bien. En revanche, une image devenue ancienne peut encore être affichée et un seek coûteux retarde la cible actuelle. L’annulation du scheduler retire les abonnés ; elle n’arrête pas le décodage déjà en cours. Sa priorité Scrub ne préempte pas les quatre jobs Thumbnail déjà lancés. Voir [DecodeScheduler.cpp:91](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/DecodeScheduler.cpp:91>).

Il faut piloter explicitement la fraîcheur des résultats : cible la plus récente, âge maximal acceptable, distance temporelle admissible, résultat exact au relâchement. Une annulation coopérative peut être vérifiée entre paquets/frames lorsque plus aucun abonné n’a besoin du calcul. Annuler aveuglément à chaque événement souris risque cependant d’empêcher toute image d’arriver ; le système doit conserver une progression visible.

L’audio est rendu muet pendant le scrubbing, mais chaque changement de position prépare encore son nouveau buffer. Reporter cette préparation au relâchement, lorsque l’aperçu audio n’est pas demandé, retire du travail concurrent. Voir [ResidentVideoPlayer.cpp:291](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/media/ResidentVideoPlayer.cpp:291>) et [QuickCanvasHost.cpp:1118](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/frontend/rendering/canvas/QuickCanvasHost.cpp:1118>).

Cette modification exige de distinguer disponibilité vidéo pendant le geste et préparation audiovisuelle : preparedAt() attend actuellement l’audio même en mode scrubbing. Supprimer seulement les appels audio pourrait donc bloquer l’état de préparation. L’audio doit être préparé à la position finale avant toute reprise effective.

Autre piste, à mesurer : chaque mouvement de souris appelle seek ; le document réévalue les médias même si la position quantifiée n’a pas changé, puis le host parcourt les lecteurs. Dédupliquer les positions et regrouper les demandes vidéo au rythme d’affichage peut diminuer ce coût. Ce n’est pas une preuve que le thread UI est le goulot principal. Voir [TimelinePanel.qml:735](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/resources/qml/app/canvas/TimelinePanel.qml:735>) et [CanvasDocument.cpp:478](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/src/backend/domain/canvas/CanvasDocument.cpp:478>).

**6. Ce que les sources publiques enseignent réellement**

| Source primaire | Constat vérifié | Application à Mouffette |
|---|---|---|
| [Manuel officiel DaVinci Resolve 20](https://documents.blackmagicdesign.com/UserManuals/DaVinci_Resolve_20_Reference_Manual.pdf) | Pages numérotées 194–196 et 216–217 : Performance Mode ajuste le traitement d’aperçu ; Proxy Media et Optimized Media fournissent des versions plus faciles à lire ; Timeline Proxy réduit la résolution à la volée ; Render Cache vise les traitements/effets coûteux. Page 706 : le nombre d’images du filmstrip dépend du zoom, un autre mode montre les extrémités. | Séparer aperçu, représentation de travail et sortie finale. Le manuel ne révèle pas les algorithmes internes du filmstrip, du cache ou de l’ordonnanceur ; aucune affirmation de « copie du moteur Resolve » n’est justifiée. |
| [Kdenlive — ClipThumbs.qml](https://github.com/KDE/kdenlive/blob/acdc35977558152740125afd3f6e69f3bc04bc07/src/timeline2/view/qml/ClipThumbs.qml#L62-L98) | Chargement asynchrone, source neutralisée hors viewport, ancienne image conservée pendant le chargement pour les thumbnails d’extrémité. | Exemple Qt concret de continuité. Cette conservation ne couvre pas toutes les images du filmstrip. Son sampling dépend aussi du zoom ; ce n’est pas une preuve de grille temporelle stable. |
| [Kdenlive — thumbnailcache.cpp](https://github.com/KDE/kdenlive/blob/acdc35977558152740125afd3f6e69f3bc04bc07/src/utils/thumbnailcache.cpp#L155-L200) | Cache mémoire LRU compté en octets, repli disque ; identité issue du média et de la frame. | Découpler la durée de vie d’une miniature de celle du clip affiché. |
| [Shotcut — thumbnailprovider.cpp](https://github.com/mltframework/shotcut/blob/61869c2392e0f3a7926972371db0f9b212a12fa0/src/qmltypes/thumbnailprovider.cpp) | Provider asynchrone, consultation du cache en base avant extraction ; précision temporelle réduite à la centiseconde pour favoriser les hits. | Exemple de clés réutilisables. La centiseconde seule serait trop fine pour résoudre le churn de zoom de Mouffette. |
| [Qt — QQuickImageProvider](https://doc.qt.io/qt-6/qquickimageprovider.html#asynchronous-image-loading) | Un provider asynchrone classique peut sérialiser les requêtes sur un seul thread par moteur ; Qt conseille QQuickAsyncImageProvider avec ordonnancement propre pour éviter ce blocage. | Remplacer le rendu actuel par Image/asynchronous ne suffirait pas. Garder un scheduler contrôlé et des tâches bornées. |
| [Qt — Scene Graph Renderer](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html) | Les atlas permettent le regroupement des dessins ; les textures C++ peuvent être créées avec TextureCanUseAtlas. Les coordonnées doivent respecter leur sous-rectangle. | Piste secondaire pour diminuer les changements de textures après correction de la stabilité. sharedImageTexture n’active pas actuellement cet indicateur. |

Le manuel Resolve a été téléchargé depuis Blackmagic et son texte consulté localement, le PDF étant trop volumineux pour l’outil de lecture web. Les références Kdenlive et Shotcut sont fixées à des commits pour rendre la recherche reproductible.

**7. Proposition de filmstrip stable pour le comportement recherché**

Le modèle conseillé est une bande attachée au temps du média source, dont le clip expose une portion. Déplacer le clip déplace sa fenêtre ; modifier son entrée change la portion exposée ; zoomer modifie la projection et la densité. Il s’agit d’une proposition pour Mouffette, pas d’une description vérifiée de l’algorithme Resolve.

Séparer trois identités : média source, échantillon temporel, position écran. La clé de la miniature ne doit contenir ni largeur du clip, ni pixelsPerMs.

Choisir des niveaux temporels emboîtés, par exemple 4 s, 2 s, 1 s, 0,5 s, puis des niveaux plus fins. À chaque niveau, les timestamps restent ancrés à la même origine du média. Passer au niveau plus fin insère des échantillons entre ceux déjà présents. Sélectionner le niveau en fonction de la densité en pixels, avec hystérésis pour éviter d’osciller au voisinage d’un seuil.

Pour une lecture sans changement de vitesse :

```text
timestamp échantillon = origineSource + k × pasDuNiveau
position locale      = (timestamp échantillon − sourceIn) × pixelsParMilliseconde
```

La largeur de chaque cellule vaut pasDuNiveau × pixelsParMilliseconde : les cellules restent ainsi contiguës. Conserver une largeur fixe tout en appliquant ces nouvelles positions provoquerait des trous ou des chevauchements. L’image est recadrée dans sa cellule en conservant son ratio ; le premier intervalle partiellement visible est découpé au bord du clip.

Le temps nominal de grille détermine la géométrie. Il est résolu vers une frame réelle via l’index de PTS pour choisir le contenu et la clé du cache, notamment en VFR ; cette résolution ne déplace pas la cellule. Plusieurs cellules peuvent partager la même frame. Éviter une formule frame = temps × fps supposé constant. Les régions où la première/dernière image est tenue doivent conserver leur sémantique actuelle.

Pendant le zoom, réutiliser le niveau déjà prêt ; remplacer progressivement quand les nouveaux samples sont disponibles. La géométrie peut évoluer chaque frame sans redécoder chaque frame. Le cache doit dédupliquer les échantillons de niveaux différents qui correspondent à la même image source.

**8. Proposition pour un scrubbing rapide**

Commencer par les corrections de scheduler et de présentation, puis mesurer ce qui reste dû au codec. Sur des GOP aussi longs, elles ne suffiront probablement pas à rendre tous les sauts exacts instantanés.

| Option | Gain recherché | Limites |
|---|---|---|
| Réutilisation/affinité de sessions, extraction par GOP, petite fenêtre avant/arrière | Éviter les seeks et redécodages identiques ; améliorer les allers-retours locaux | Borner les sessions et les buffers ; plusieurs centaines de frames pleine définition consommeraient beaucoup de RAM |
| Proxy d’édition à résolution adaptée, intra ou GOP court, créé en arrière-plan | Accès aléatoire bien moins coûteux, particulièrement en marche arrière | Temps et espace de génération ; correspondance PTS/VFR, rotation et couleurs à garantir ; ne pas rebloquer l’import sur sa création |
| Aperçu approximatif pendant déplacement très rapide, raffinement au ralentissement puis frame exacte au relâchement | Afficher rapidement une réponse utile | Compromis de précision temporelle à maîtriser ; les thumbnails de 192 × 108 ne remplacent pas un aperçu vidéo de bonne qualité |
| Décodage matériel avec FFmpeg, par exemple VideoToolbox sur macOS | Réduire le coût CPU selon codec/machine | Les dépendances temporelles restent ; mesurer seeks, copies et transferts, pas seulement le débit séquentiel |

FFmpeg fournit un exemple officiel d’initialisation du décodage matériel et de transfert éventuel vers des frames CPU. Son intégration dans les QVideoFrame/QRhi existants doit être conçue explicitement ; un aller-retour systématique GPU → CPU peut annuler une partie du gain. [Exemple FFmpeg hw_decode](https://www.ffmpeg.org/doxygen/trunk/hw_decode_8c-example.html).

Pour cette application, le proxy doit être un dérivé d’édition facultatif et progressif, indépendant de l’original conservé pour l’identité, les transferts et la qualité finale. Un simple redimensionnement après décodage du H.264 original n’enlève pas le coût de reconstruction de son GOP.

Exemple concret : le générateur de proxies de Shotcut configure généralement g=1 et bf=0, avec des variantes selon l’encodeur, puis lance un job en arrière-plan. Cela illustre l’intérêt de choisir une représentation pour l’accès aléatoire, au-delà de sa seule résolution. [Implémentation Shotcut](https://github.com/mltframework/shotcut/blob/61869c2392e0f3a7926972371db0f9b212a12fa0/src/proxymanager.cpp#L350-L373).

**9. Ordre de mise en œuvre et critères de réussite**

| Priorité | Travail | Vérification attendue |
|---|---|---|
| P0 | Conserver images/textures jusqu’au remplacement ; protéger les ressources visibles ; utiliser le poster | Aucun trou après premier affichage pendant zoom/scroll/trim, hors changement de source ou récupération mémoire explicitement contrôlée |
| P0 | Requêtes incrémentales ; visible avant voisinage ; réserver de la capacité interactive ou borner les jobs d’arrière-plan | Scrub non retenu derrière une longue série de miniatures ; travail abandonné et attente de file mesurés |
| P1 | Grille temporelle stable, niveaux emboîtés et hystérésis | Identités réutilisées au zoom ; densité correcte ; pas de glissement arbitraire au trim |
| P1 | Publication progressive d’aperçus et réutilisation du premier décodage | Première image et premières vignettes disponibles avant validation EOF |
| P1 | Fraîcheur des résultats de scrub, annulation coopérative, préparation audio différée | Faible âge des images présentées ; dernier seek exact au relâchement ; aucune famine pendant drag continu |
| P2 | Proxy/cache GOP/décodage matériel selon profilage | Gain établi sur originaux long-GOP, vidéos 4K et plusieurs médias |
| P2 | Cache disque, atlas, budget d’uploads GPU | Réouverture plus rapide, frame-time stable et mémoire bornée |

Le test existant thumbnailRepetitionZoomViewportAndRelease utilise une image PNG : il ne peut pas révéler l’invalidation temporelle des thumbnails vidéo. Les tests vidéo attendant un état final via QTRY peuvent passer malgré des trous transitoires. Voir [tst_MediaFrameItem.cpp:250](</Users/maxbraudel/Personnel/Toute ma vie/30 Projets/Développement/Mouffette/Versions/2026-03-23_16-22-44Z – Personnel Dev/client/tests/qt/tst_MediaFrameItem.cpp:250>).

La prochaine validation doit observer chaque frame pendant un geste, sur cache froid et chaud, et avec concurrence thumbnails/scrubbing : couverture du filmstrip, premières vignettes, âge du résultat affiché, latence de la dernière cible, temps de file/décodage/conversion/upload, demandes périmées, taux de hits et mémoire CPU/GPU. Distinguer livraison au QVideoSink et présentation réelle à l’écran.

Exemples d’objectifs à qualifier sur une machine et un corpus définis : interface à 60 Hz, réponse visuelle au scrub P95 sous 50 ms sur représentation d’édition prête, frame exacte après relâchement P95 sous 100 ms. Ce sont des cibles de conception, pas des performances établies de cette version ni des garanties pour tout média.

Le QML Profiler et les diagnostics QSG_RENDER_TIMING / QSG_VISUALIZE permettent de distinguer calcul QML, synchronisation, textures et rendu. Qt rappelle qu’un affichage à 60 Hz dispose d’environ 16 ms par frame. [Conseils de performance Qt Quick](https://doc.qt.io/qt-6/qtquick-performance.html), [diagnostics du scene graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html).

Les premières corrections peuvent conserver le renderer Qt Quick, la mutualisation des assets et le lecteur partagé. La priorité est d’assurer la continuité visuelle et de retirer les calculs périmés du chemin interactif ; les représentations de travail plus faciles à décoder viennent ensuite traiter le coût des vidéos long-GOP.
