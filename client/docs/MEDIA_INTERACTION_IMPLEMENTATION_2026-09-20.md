# Import, vignettes et scrubbing : corrections et validation

20 septembre 2026. Implémentation dans le workspace existant, en conservant les modifications qui étaient déjà présentes. L’[audit initial](MEDIA_INTERACTION_AUDIT_2026-09-20.md) reste un état historique du problème. Le contrat technique courant est décrit dans [MEDIA_RESIDENCY.md](MEDIA_RESIDENCY.md).

Le comportement d’import décrit ci-dessous est historique : les aperçus anticipés
sont désormais conservés en interne, le canvas et la timeline affichent un skeleton
jusqu’à la disponibilité du média, et Play local avance sans attendre les imports.
Les médias prêts rejoignent la position courante ; la préparation distante reste
complète. Voir le contrat courant cité ci-dessus.

## Comportement implémenté

| Problème | Correction |
|---|---|
| L’image attend la validation complète du fichier | Publication d’un poster natif et d’une collection progressive et immuable de petites vignettes. Le canvas et la timeline peuvent les afficher avant `ready`. La validation intégrale reste obligatoire pour Play et la diffusion distante. |
| Les vignettes changent de cible à chaque variation du zoom | Grille ancrée dans le temps source, niveaux emboîtés et hystérésis. La géométrie des cellules reste continue ; le contenu est résolu par les timestamps réels, y compris en VFR et après trim. |
| Des trous apparaissent en attendant les nouvelles images | Conservation des images visibles jusqu’au remplacement, fallback provenant du même média, requêtes incrémentales, priorité aux cellules visibles. |
| Les thumbnails encombrent le décodage interactif | Pool borné, capacité réservée aux demandes interactives, annulation coopérative du démux/décodage, sessions réutilisées. Une annulation n’efface pas la requête de remplacement portant la même clé. |
| Le scrubbing redécode continuellement les longs GOP | Cache d’édition indépendant : images JPEG à accès direct, bord long de 960 pixels, préparées progressivement par un worker de faible priorité. Le cache ne bloque pas l’import et se suspend pendant le geste, Play ou une restriction mémoire. |
| Des positions dépassées et du travail audio parasitent le geste | Regroupement des demandes au rythme de présentation, suivi de la dernière cible et du sens du geste, préparation audio différée. L’image originale et la préparation audiovisuelle reprennent au relâchement. |
| Les caches deviennent une source de consommation permanente | LRU RAM et disque bornés, destruction/annulation des propriétaires, partage des pixels et textures, comptage des pixels visibles même après leur éviction du LRU. Les sessions de décodage inactives expirent. |

Les caches disque sont des dérivés locaux jetables : 64 Mio pour les vignettes, 512 Mio pour le scrubbing, au maximum 12 000 fichiers par cache. Les clés comprennent l’identité originale, le flux, le timestamp, la durée, la géométrie, la rotation et la version du format. Les écritures sont atomiques ; une entrée manquante ou endommagée peut être régénérée, y compris après la fin d’un premier parcours du média.

Les références visibles des vignettes sont limitées à 2 Mio par bande. Les LRU optionnels conservent au plus 32 Mio de vignettes et 64 Mio de frames ; ces limites ne représentent pas la totalité de la mémoire vidéo, puisqu’un lecteur ou un renderer peut encore posséder une frame requise. La récupération critique libère aussi les pins visibles et annule les calculs optionnels.

Le proxy JPEG est réservé aux originaux SDR de profondeur au plus 8 bits, sans alpha. HDR, alpha, formats non caractérisés et représentations déjà intra gardent leur chemin natif. Les originaux, leurs hashes, les fichiers du projet et les transferts ne sont pas remplacés par les dérivés d’édition. L’ordre LRU du disque est maintenu progressivement : remplir le quota ne provoque pas un nouveau tri de 12 000 entrées à chaque frame écrite.

## Validation native et automatisée

Compilation locale : Apple M1, macOS 26.1, Qt 6.11.2, FFmpeg 8, configuration `macos-debug`. Les suites graphiques ont été exécutées avec le renderer natif ; les tests bas niveau utilisent aussi la plateforme Qt offscreen. Les avertissements existants sur les API privées Qt Multimedia demeurent.

Les validations couvrent notamment :

- La couverture de la bande vidéo sur 72 frames successives de zoom, déplacement et trim, les niveaux stables, les purges ordinaires et la libération sous pression.
- La publication avant EOF, le partage du poster et des vignettes entre snapshots, le changement de source, les générations retirées, la corruption en fin de média et le refus mémoire.
- Les remplacements après annulation, les abonnements visibles conservés, la libération des allocations après des cycles de lecteurs et l’absence de nouvelle préparation audio durant un drag qui appelle aussi `pause()`.
- Le retour à la frame originale au relâchement, la reprise de Play, la conservation des positions de lecture et les réservations de préparation.
- Les caches endommagés/évincés, leur régénération et la libération du dernier propriétaire.
- Le rendu natif, la rotation, le recadrage, le Retina, les atlas et l’alpha ; la résolution 4K et les métadonnées HDR BT.2020/PQ sont vérifiées dans les buffers.
- Les barrières de préparation distante et l’absence d’autorisation implicite créée par le nouvel aperçu local.

| Suite / passage ciblé | Résultat |
|---|---|
| `ResidentMedia`, suite complète avec fixtures 4K/HDR/alpha | 46 PASS, 0 échec, 0 skip |
| `MediaResidencyManager`, suite complète | 40 PASS, 0 échec, 2 cas nécessitant une invocation séparée |
| Backend volontairement indisponible, invocation isolée | 3 PASS, 0 échec |
| `MediaFrameItem`, suite complète GPU natif | 19 PASS, 0 échec |
| `VideoPlaybackBackend`, suite complète native | 19 PASS, 0 échec |
| `TimelineController`, 10 fonctions ciblées avec leurs variantes | 14 PASS, 0 échec |
| `MediaEditingCache`, dont limite de 12 000 fichiers et régénération | 7 PASS, 0 échec |
| Barrières distantes et perte de fenêtre, deux passages ciblés | 5 PASS et 4 PASS, 0 échec |
| Préparation distante d’un curseur vidéo après le dernier ajustement du lookahead | 3 PASS, 0 échec |
| Import natif du fichier utilisateur avec captures | 3 PASS, 0 échec |

Les comptes Qt incluent `initTestCase` et `cleanupTestCase` ; ils ne doivent pas être additionnés comme autant de scénarios distincts. Les deux cas ignorés de la suite mémoire sont le fichier externe optionnel et le backend volontairement indisponible. Ils ont été traités par les invocations dédiées indiquées ci-dessus ; l’import externe a aussi été vérifié dans le canvas natif. `git diff --check` passe.

Un test d’import natif sur `VID_20260920_013247.mp4` a attendu le poster, la fin du fondu et un nouveau `frameSwapped`, puis capturé le canvas alors que `residencyReady` était encore faux. La capture a été inspectée : la vraie vidéo est visible. Le test a observé ce point à 1 133 ms après le dépôt, sans attendre la validation intégrale. Cette valeur inclut le QML, le fondu et les attentes du test ; elle ne mesure pas la latence physique de l’écran.

## Reproduire les mesures

Sur le fichier fourni, 2 111 frames H.264 à longues dépendances temporelles, un passage isolé de la version finale donne :

| Mesure | Cache d’édition absent | Cache d’édition préparé |
|---|---:|---:|
| Latence P95 des 24 sauts jusqu’au sink | 679,1 ms | 18,3 ms |
| Images livrées pendant les 180 déplacements | 5 | 179 |
| Images livrées par seconde pendant le geste | 1,4 | 50,0 |
| Écart P95 entre timestamp présenté et cible mouvante, en temps source | 20 197 ms | 32 ms |
| Préparation exacte après le geste sous charge thumbnails | 772,5 ms | 612,3 ms |

Le premier aperçu est produit par le décodeur en **102 ms**, la validation intégrale termine en **5 756 ms**. Après activation du générateur, les **2 111 dérivés** sont disponibles en **15 929 ms** supplémentaires ; cette génération ne bloque pas l’UI. Les buffers de frames suivis reviennent à **0 octet** après libération des lecteurs, assets et caches, puis expiration des sessions.

Le rapport d’environ **37×** concerne les sauts vers le sink lorsque le cache d’édition est prêt. Il ne signifie pas un gain équivalent pour toutes les opérations. Le test parcourt presque toute la vidéo en quelques secondes : à froid, les frames intermédiaires peuvent être très éloignées de la cible en temps source malgré une ancienneté de requête bornée à une seconde. Elles permettent une progression, sans constituer une prévisualisation instantanée et exacte. Au relâchement, l’aperçu déjà affiché reste visible pendant le décodage de la pleine définition.

Ces chiffres correspondent à un passage Debug sur cette machine, pas à une distribution de plusieurs sessions, une comparaison avant/après de toute l’ancienne application ou une mesure physique à l’écran. Le cache disque de l’application est neuf pour ce benchmark, mais le cache de fichiers du système n’a pas été purgé. Les [résultats structurés](benchmarks/media-interaction-m1-2026-09-20.json) gardent les valeurs brutes et le contexte.

Le même chemin a été mesuré séparément sur la fixture synthétique **4K, 2 secondes, 60 frames** : P95 des sauts **863,5 ms sans dérivé → 18,2 ms avec dérivés**, génération de 60 proxies en **1 545 ms**, retour exact après geste **268,9 ms** avec cache prêt, et **0 octet** de buffers suivis après libération. Le geste délivre 87 images avec cache : les événements adjacents demandent souvent la même frame sur cette source très courte, donc ce compte ne se compare pas directement à celui de la vidéo de 71 secondes. Cette fixture vérifie le chemin 4K ; elle ne représente pas à elle seule les charges longues ou multiclips.

Le benchmark dédié utilise le chemin actuel `retainOriginalVideo=true`, un cache disque isolé et jetable, et rapporte la livraison au `QVideoSink`. Il mesure 24 sauts répartis sur la durée, puis 180 événements espacés de 16 ms dans les deux sens, avec 47 demandes de vignettes concurrentes. Le geste se termine à l’intérieur du média, afin de ne pas masquer le coût du retour exact avec le poster déjà prêt à zéro. Le nombre de proxies disponibles et les buffers suivis après libération sont également rapportés.

```sh
cmake --build client/out/build/macos-debug --target media_interaction_benchmark -j 4
QT_QPA_PLATFORM=offscreen client/out/build/macos-debug/media_interaction_benchmark VID_20260920_013247.mp4
```

Les anciens tableaux de `media_engine_benchmark` utilisent une autre représentation de travail et ne doivent pas être présentés comme une référence directe pour les originaux à longs GOP.

## Limites de portée

Le cache accélère les accès aux images déjà générées ; un premier saut dans une région non préparée reste dépendant du codec original. Le relâchement conserve l’aperçu pendant le retour à la pleine définition. Le décodage matériel n’a pas été ajouté. Ces choix ne garantissent pas une latence identique sur tous les codecs, toutes les machines ou un nombre arbitraire de vidéos simultanées.

Cette validation locale ne constitue pas une qualification Windows, une mesure de scanout de l’écran, un étalonnage HDR, ni une garantie universelle de 60 images par seconde. Les caches et le comportement après annulation sont couverts séparément des mesures de vitesse.
