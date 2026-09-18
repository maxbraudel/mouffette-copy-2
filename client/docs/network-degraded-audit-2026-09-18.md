# Investigation des uploads sur une connexion dégradée — 18 septembre 2026

Périmètre : logs fournis (510 lignes logiques), code local au commit `92ba8ed`,
protocole **v11**, politique réseau **v4**. Les versions exécutées sur les deux
machines et les éventuelles surcharges de configuration du serveur distant ne
sont pas établies par cet extrait. Aucune modification du comportement de
production n'a été faite pour cet audit.

**Conclusion : le transport des fichiers peut retarder les heartbeats qui
autorisent ce même transport. Le serveur peut alors couper une connexion sur
laquelle des données continuent d'arriver. La reprise conserve les octets, mais
elle ne résout pas ce conflit : elle peut retomber dans la même situation jusqu'à
l'expiration de la session et au nettoyage des médias.**

## Ce que les logs établissent

| Lignes | Observation | Interprétation et limite |
| --- | --- | --- |
| 62, 408, 457 | Perte du canal dédié ; nouvelle tentative dans 4 389 / 4 831 / 4 577 ms | Le transport d'upload n'est pas stable. Le code peut utiliser la connexion de contrôle en secours immédiatement ; ces délais ne signifient donc pas nécessairement une attente d'autant pour les données. |
| 299–310, 360–370, 411–421, 461–472 | Quatre déconnexions du contrôle, suivies d'authentifications | Il ne s'agit pas seulement d'un changement de couleur du badge. |
| 303 | `Network event loop delayed lagMs 3619` | Un passage du watchdog a été retardé de 3,619 s. L'extrait ne permet pas de distinguer un blocage du thread, un manque de CPU ou une suspension de la machine. |
| 359, 404–407, 455–456 | `upload_ready`, reprises `upload_resume_ready`, puis `lease_expired` | Le démarrage et certaines reprises fonctionnent ; l'autorisation de session devient néanmoins indisponible. Ni les offsets ni le message rejeté ne sont loggés ici. |
| 255, 465 | `Unmarked all files for client` | Les marqueurs locaux de présence distante sont invalidés. Cela ne prouve pas, à lui seul, la suppression physique achevée sur le destinataire. |
| 349–354 | `remoteFilesPresent: true`, `hasActiveUpload: false` | L'interface croit encore avoir des médias distants. Une incohérence correspondante existe dans le chemin de fermeture décrit plus bas. |
| Ensemble | 297 `media_residency`, 40 `remote_session_terminating`, 40 `remote_session_closed` | Sans timestamps ni identifiants de session, impossible de déduire le débit des messages, 40 sessions distinctes ou une boucle infinie. |

Le badge `Degraded` **recouvre volontairement de vraies déconnexions, les nouvelles
connexions TCP, l'authentification et la synchronisation**, pendant la fenêtre de
récupération. Voir `ConnectionManager::getConnectionStatus`, ligne 225 de
[ConnectionManager.cpp](../src/backend/managers/network/ConnectionManager.cpp).
Un utilisateur peut donc voir seulement `Connected ↔ Degraded` malgré les
coupures présentes dans les logs. Cela précise la réponse précédente : le
badge seul ne garantit ni la validité de la session ni la conservation future
des médias.

## 1. Critique : le canal de secours peut provoquer sa propre déconnexion

Dans [WebSocketClient.cpp](../src/backend/network/WebSocketClient.cpp),
`beginUploadSession` (582) choisit le contrôle si le canal dédié n'est pas prêt,
et garde ce choix pendant le transfert. Le handler `uploadTransportLost` dans
[UploadManager.cpp](../src/backend/network/UploadManager.cpp) (665) suspend puis
essaie immédiatement de reprendre, notamment sur le contrôle.

Un envoi peut placer huit blocs de **128 Kio**, soit **1 Mio de données utiles**,
avant les accusés de réception distants. Le JSON/base64 représente environ
**1,34 Mio sur le réseau**. La fenêtre est bornée en octets, sans adaptation au
débit ni au temps pendant lequel elle occupe le réseau.

Les heartbeats sont des messages applicatifs sur ce même flux ordonné. Côté
serveur, seul le heartbeat renouvelle `lastHeartbeatMonotonicAt`. Les blocs
reçus entre-temps n'empêchent pas `clientLeaseExpired` de devenir vrai après
1 500 ms. Voir [server.js](../../server/server.js), lignes 1564, 1605 et 1746.

**Reproduction sur le véritable serveur local, sans simuler le watchdog :**
à **256 Kio/s en émission**, avec le secours sur le contrôle, le serveur a fermé
le socket après **1 533 ms**, code **1001**, motif **`Transport heartbeat timeout`**.
Le destinataire avait déjà reçu **262 144 octets** du Mio envoyé. La session est
passée en `Grace`. Il n'y avait ni perte de paquets injectée ni arrêt volontaire
des heartbeats ; ils étaient simplement derrière les données.

Ce chemin est particulièrement pertinent pour ces logs, qui montrent des pertes
répétées du canal dédié. Il n'est toutefois pas possible de prouver, avec cet
extrait, quel socket chaque reprise de l'ami a effectivement sélectionné.

## 2. Critique : le canal dédié ne sépare pas la réception des données du contrôle

Même lorsque l'expéditeur utilise son canal dédié, `handleUploadChunk` transmet
les blocs au destinataire avec `sendToEndpoint`. Cette méthode écrit sur
**`client.ws`**, sa connexion de contrôle, pas sur son socket d'upload.
Voir [server.js](../../server/server.js), lignes 4152, 4185 et 2762.

Un proxy TCP local limitant uniquement ce sens, sans modifier les messages,
donne les résultats suivants pour la même fenêtre de 1 Mio :

| Débit descendant | Premier bloc complet | Heartbeat ACK suivant | Seuil de coupure transport : 1,5 s | Budget maximal de preuve de session : 4,5 s |
| --- | ---: | ---: | --- | --- |
| 512 Kio/s | 376 ms | 2 805 ms | Non franchi dans cette mesure | Non franchi dans cette mesure |
| 256 Kio/s | 747 ms | 5 592 ms | Non franchi : des blocs arrivent régulièrement | **Franchi** |
| 64 Kio/s | 2 786 ms | 22 261 ms | **Franchi dès le premier bloc** | **Franchi** |

Le test emploie le vrai relais et de vrais WebSockets. Son destinataire est un
client de diagnostic JS qui continue de lire après les délais afin de mesurer
l'attente complète. **Les expirations Qt de ce tableau sont déduites des délais
mesurés et du code ; ce tableau n'est pas un test de transfert complet avec le
client graphique.** Le relais considère encore la session utilisable à la fin,
car les heartbeats dans le sens retour continuent d'arriver.

La distinction est importante à 256 Kio/s : les blocs maintiennent la fraîcheur
du transport Qt via `noteServerContact`, mais ne renouvellent pas la preuve de
session, véhiculée notamment par `heartbeat_ack.sessionStates`. Le client peut
donc recevoir des données continuellement tout en perdant cette session.
`checkSessionRecoveryDeadlines` (1411) s'exécute avant le traitement des nouveaux
messages (2245) ; un ACK tardif ne ressuscite pas une session déjà expirée.

Le test Qt existant `localProofExpiryClosesAStillHealthyServerSession` confirme
séparément que l'expiration d'une preuve termine la session même lorsque les
deux transports restent connectés. Il a été exécuté avec succès pendant l'audit.

## 3. Important : délais très courts et couplage entre contrôle, upload et cache

La configuration actuelle dans [server/.env](../../server/.env) et
[config.js](../../server/config.js), ligne 124, impose :

- heartbeat toutes les **750 ms** ; interruption après **deux intervalles**, soit **1 500 ms** ;
- récupération de session de **3 000 ms**, comptée depuis la détection ;
- preuve normale plafonnée à environ **4 500 ms** depuis le dernier contact pertinent,
  moins les marges d'horloge et le temps déjà écoulé ;
- timeout d'inactivité d'upload de **45 s**, acceptation/validation de **30 s**.

Les 30/45 secondes ne protègent donc pas contre les coupures à 1,5/4,5 seconde.
Le client et le serveur peuvent terminer la session bien avant un timeout
spécifique au transfert. Reconnexion, authentification, RESUME et application de
l'état par les deux participants doivent tenir dans le budget restant.

Le retard local de 3,619 s constaté est particulièrement pénalisant. Le transport,
ses timers et `UploadManager` résident dans le thread de `ApplicationRuntime`
([ApplicationRuntime.cpp](../src/backend/runtime/ApplicationRuntime.cpp), ligne
322). Les écritures entrantes et une partie des validations sont bien déportées
dans des workers ; ce n'est pas vrai de tout le traitement des événements,
de la sérialisation, des mises à jour d'interface et des lectures de blocs sortants.
Un profilage Windows est nécessaire pour attribuer le retard observé à une
opération précise. Le nombre de `media_residency` ne suffit pas à l'expliquer.

Après expiration, la session et son cache sont traités comme terminaux :
`remoteSessionRecoveryExpired` déclenche les terminaisons d'upload et le nettoyage
dans `ApplicationRuntime`. Une nouvelle session peut s'ouvrir automatiquement,
mais elle ne reprend pas magiquement le cache de l'ancienne session révoquée.
C'est ce couplage qui transforme des interruptions répétées en perte des uploads
en cours et invalidation de médias précédemment transférés.

## 4. Bug d'interface confirmé par lecture du code : présence distante périmée

`clearRemoteSessionRuntimeState` vide `knownRemoteFileIds`, remet les médias à
« non uploadé » et appelle `clearUploadTracking`, mais **ne remet pas
`workspace.upload.remoteFilesPresent` à `false`**.
`ClientWorkspaceController::clearUploadTracking` préserve lui aussi ce booléen.

Références : [ApplicationRuntime.cpp](../src/backend/runtime/ApplicationRuntime.cpp),
3059 ; [ClientWorkspaceController.cpp](../src/backend/controllers/ClientWorkspaceController.cpp),
266. Le booléen est lu par le bouton d'upload et par le view model.

Une fermeture après un upload réussi peut donc laisser la présentation « médias
distants présents » alors que les inventaires ont été vidés. Cela correspond
au décalage des logs, sans prouver que ce booléen était l'unique cause de ce clic.
L'inventaire réel est consulté séparément pour construire un nouveau transfert :
ce défaut d'affichage n'explique pas les coupures réseau reproduites.

La progression globale utilise aussi le maximum entre octets localement envoyés
et progression distante (`emitEffectiveProgressIfChanged`, UploadManager.cpp,
3191). Sur une liaison lente, le pourcentage peut donc devancer les octets
effectivement reçus. Ce n'est pas une confirmation de succès du transfert.

## 5. Trafic auxiliaire et observabilité à améliorer

`media_residency` rapporte la préparation/mémoire des médias, pas l'acquittement
de leurs blocs d'upload. Chaque modification d'un propriétaire peut publier un
snapshot complet des médias de sa session (`publishResidency`, 896). Il n'y a
pas de regroupement temporel ni de suppression des snapshots identiques à cet
endroit. La progression du décodeur est limitée par pas de 0,5 %, mais pas par
nombre de messages par seconde : un décodage rapide peut donc produire beaucoup
de snapshots rapprochés. À la réception, chaque snapshot efface puis recrée les
états distants et émet plusieurs notifications.

Les 297 lignes sont compatibles avec cette mécanique ; **elles ne prouvent pas
une boucle infinie**. Limiter/regrouper cette télémétrie réduirait néanmoins la
charge sur la même connexion de contrôle. Références : UploadManager.cpp 268,
896, 915 ; MediaResidencyManager.cpp 393, 699 ; MediaDecoder.cpp 636 ; server.js
2061. Le log de chaque `media_residency` reste également activé, contrairement
à celui des blocs et heartbeats.

Les répétitions `terminating/closed` ont une autre explication normale :
`replayTerminalStateForClient` rejoue les fins de sessions conservées dans
l'historique après une nouvelle connexion. Les accusés d'application sont liés à
la génération de transport ; une nouvelle génération peut imposer un nouveau
rejeu (server.js 2606). Il ne faut donc pas les compter comme autant de nouvelles
fermetures. Leur volume ajoute toutefois du travail à la récupération.

Les logs client actuels masquent les informations nécessaires pour distinguer
ces cas : timestamp monotone, session/upload/génération, motif de fermeture du
socket, canal choisi lors de la reprise, offset durable avant/après, âge de la
dernière preuve et temps de file d'attente. Ajouter ces données et résumer les
messages fréquents est préférable à simplement activer davantage de logs bruts.

## Reproduction et validation

Le [script de diagnostic](../../server/diagnostics/slow-link-probe.js) démarre
uniquement un serveur et un proxy de test sur `127.0.0.1`, puis les ferme.
Il utilise des identités éphémères, des octets synthétiques et les dépendances
Node déjà installées du projet. Il ne valide/décode pas un média complet.

Depuis la racine du dépôt :

```sh
node server/diagnostics/slow-link-probe.js 256 uplink-fallback
node server/diagnostics/slow-link-probe.js 256
node server/diagnostics/slow-link-probe.js 64
node server/diagnostics/slow-link-probe.js 512
```

La ligne `AUDIT_RESULT` fournit les mesures JSON. Le scénario `uplink-fallback`
caractérise le défaut actuel et vérifie la fermeture ; ce n'est pas un test
affirmant que cette fermeture est le comportement souhaitable.

Vérifications exécutées :

- `npm test` : **16 scripts serveur réussis**, dont le soak de récupération.
- Build des cibles Qt concernées : à jour, succès.
- `tst_UploadRemovalSecurity` : **27 passages, aucun échec**.
- `tst_ClientConnectionFlow` : **73 passages, aucun échec**.
- Intégration Qt/Node : expiration de preuve sur transport sain, reprise depuis
  offsets durables, pertes d'ACK/readiness : **8 passages, aucun échec**.
- Les nombres Qt incluent initialisation et nettoyage des suites.

Ces tests montrent que les invariants de reprise testés passent. Ils ne couvrent
pas à eux seuls un débit faible continu, des heartbeats pris derrière les données
et l'interface Windows de l'ami. Aucune cause spécifique à son fournisseur
d'accès, au VPN, au disque ou à son ordinateur n'est établie ici.

## Ordre de correction proposé

1. **Protéger le contrôle dans les deux sens.** Séparer aussi les payloads vers
   le destinataire ; rendre le secours compatible avec les deadlines, avec
   contrôle prioritaire entre petits blocs et une fenêtre adaptée au débit.
   Deux sockets ne dispensent pas de maîtriser la saturation de la liaison.
2. **Définir des budgets distincts et cohérents.** Détection d'une connexion
   suspecte, récupération de commande/lecture et conservation d'un transfert
   reprenable ont des besoins différents. Une rétention de cache plus longue
   devra rester liée à l'identité autorisée et aux générations, sans ressusciter
   une session révoquée ni relancer une lecture automatiquement.
3. **Corriger l'état utilisateur.** Réinitialiser la présence distante sur la
   terminaison ; afficher explicitement upload suspendu/reprise/expiration et
   une progression fondée sur les octets confirmés.
4. **Réduire le trafic auxiliaire et mesurer le retard du thread.** Regrouper les
   snapshots de résidence, garder les changements d'état essentiels immédiats,
   ajouter les diagnostics corrélés et profiler le cas Windows.
5. **Transformer ces reproductions en critères de réussite.** Transfert complet
   et hash exact sous débits limités dans les deux sens, puis latence/gigue,
   pertes du canal dédié, reprises successives et pauses de la boucle UI.

Augmenter uniquement le délai de récupération peut réduire les échecs, mais
ne supprime ni la coupure du transport à 1,5 seconde ni l'attente des heartbeats
derrière les blocs. La correction prioritaire porte sur ce conflit de transport.
