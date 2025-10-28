# Scénarios de Test - Reconnexion & Persistence

## Phase 3 - Tests End-to-End

### Scénario 1 : Upload puis Reconnexion Simple
**Objectif** : Vérifier que les fichiers uploadés persistent après reconnexion

**Étapes** :
1. Lancer serveur + 2 clients (A et B)
2. Client A upload 2-3 fichiers vers Client B
3. Attendre confirmation upload (100%)
4. Killer Client A (Ctrl+C)
5. Relancer Client A
6. Sur Client A, sélectionner Client B dans la liste
7. **Vérifier** : Les fichiers uploadés apparaissent toujours sur la scène

**Résultat attendu** :
- ✅ Client A reçoit `state_sync` avec ideaId + fileIds
- ✅ Canvas affiche les médias restaurés
- ✅ Upload button montre "Unload from Client" (état actif)

---

### Scénario 2 : Reconnexion avec Nouveau Session ID
**Objectif** : Vérifier que le serveur reconnaît le client avec son persistentId malgré nouveau sessionId

**Étapes** :
1. Client A connecté et enregistré (noter son sessionId dans logs serveur)
2. Killer Client A
3. Relancer Client A (aura nouveau sessionId)
4. **Vérifier logs serveur** : 
   - `Client session reconnecting: <persistentId>`
   - Upload tracking maintenu

**Résultat attendu** :
- ✅ Serveur merge ancien + nouveau sessions
- ✅ `clientFiles` Map conserve les données
- ✅ Nouveau sessionId dans `clients` mais même persistentId

---

### Scénario 3 : Deux Clients sur Même Machine
**Objectif** : Vérifier que `--instance-suffix` crée bien des identités distinctes

**Étapes** :
1. Lancer Client A : `./run.sh`
2. Lancer Client B : `MOUFFETTE_INSTANCE_SUFFIX=instance2 ./run.sh`
3. **Vérifier logs clients** : 
   - Différents `persistentClientId`
   - Différentes clés de storage QSettings
4. Upload A→B puis B→A
5. Killer les deux clients
6. Relancer avec même suffixes
7. **Vérifier** : Chacun restaure ses propres uploads

**Résultat attendu** :
- ✅ Deux persistentIds distincts
- ✅ Pas de collision dans serveur.clientFiles
- ✅ Chaque client restaure sa propre scène

---

### Scénario 4 : Upload Interrompu + Reconnexion
**Objectif** : Vérifier nettoyage correct des uploads partiels

**Étapes** :
1. Client A commence upload vers Client B
2. Killer Client A pendant l'upload (50%)
3. **Vérifier Client B** : Upload annulé/nettoyé
4. Relancer Client A
5. **Vérifier** : Pas de fichiers orphelins

**Résultat attendu** :
- ✅ Serveur nettoie `uploads` Map pour uploadId interrompu
- ✅ Client B ne garde pas de fichiers partiels
- ✅ Client A ne tente pas de reprendre l'ancien upload

---

### Scénario 5 : Validation ideaId Obligatoire
**Objectif** : Vérifier que serveur rejette messages sans ideaId

**Étapes** :
1. Modifier temporairement client pour envoyer `upload_start` sans ideaId
2. **Vérifier logs serveur** : 
   - `⚠️ upload_start missing required fields`
   - Message d'erreur retourné au client

**Résultat attendu** :
- ✅ Serveur rejette avec erreur explicite
- ✅ Upload ne démarre pas
- ✅ Pas de corruption de `clientFiles` Map

---

### Scénario 6 : Client Sans PersistentId
**Objectif** : Vérifier que client legacy (sans persistentId) est rejeté

**Étapes** :
1. Modifier temporairement client pour ne pas envoyer clientId dans register
2. Tenter connexion
3. **Vérifier logs serveur** :
   - `⚠️ register missing clientId`
   - Erreur retournée

**Résultat attendu** :
- ✅ Serveur rejette register
- ✅ Client n'apparaît pas dans client_list
- ✅ Pas d'accès aux fonctionnalités

---

## Validation Globale

**Tous scénarios passent** = Phase 3 complète ✅

**Métriques de succès** :
- 0 crash client lors reconnexion
- 0 perte de données upload
- 0 collision d'identité entre instances
- 100% des uploads restaurés correctement
- Tous les rejets serveur avec messages d'erreur clairs

---

## Prochaines Étapes (Phase 4)

Une fois Phase 3 validée :
1. Extraire `SessionManager` class
2. Découper `FileManager` en 3 services
3. Injection de dépendances
4. Tests unitaires automatisés
