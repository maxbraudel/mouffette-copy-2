# 🔐 RAPPORT COMPLET - SYSTÈMES D'IDENTIFICATION MOUFFETTE

**Date**: 29 Octobre 2025  
**Application**: Mouffette - Système de partage multimédia temps réel  
**Auteur**: Analyse technique approfondie

---

## 📋 TABLE DES MATIÈRES

1. [Vue d'ensemble](#vue-densemble)
2. [Identification des Clients](#identification-des-clients)
3. [Identification des Médias](#identification-des-médias)
4. [Identification des Fichiers](#identification-des-fichiers)
5. [Identification des Sessions](#identification-des-sessions)
6. [Identification des Idées (Scènes)](#identification-des-idées-scènes)
7. [Flux de Connexion Détaillé](#flux-de-connexion-détaillé)
8. [Diagramme Complet](#diagramme-complet)
9. [Cas d'Usage Critiques](#cas-dusage-critiques)

---

## 1. VUE D'ENSEMBLE

L'application Mouffette utilise **5 systèmes d'identification distincts et complémentaires** :

```
┌─────────────────────────────────────────────────────┐
│                    MOUFFETTE                         │
├─────────────────────────────────────────────────────┤
│  1. Persistent Client ID  (identité stable client)  │
│  2. Session ID            (session réseau éphémère) │
│  3. Media ID              (item canvas unique)      │
│  4. File ID               (fichier unique)          │
│  5. Idea ID               (scène/projet)            │
└─────────────────────────────────────────────────────┘
```

**Principe clé** : Chaque système d'identification répond à un besoin spécifique et ne doit **jamais** être confondu avec un autre.

---

## 2. IDENTIFICATION DES CLIENTS

### 2.1 Persistent Client ID

**Rôle** : Identité **STABLE** et **PERMANENTE** d'une installation client sur une machine.

#### Génération
**Localisation** : `MainWindow.cpp` ligne 656

```cpp
QString persistentClientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
// Exemple : "3f8c7d2a-5b1e-4f6d-9a2c-1b3d4e5f6g7h"
```

#### Stockage
**Méthode** : `QSettings` (persiste sur disque)

**Clé composite** :
```cpp
QString settingsKey = "persistentClientId_{machineId}_{installHash}_{instanceSuffix}"
```

Composants de la clé :
1. **machineId** : `QSysInfo::machineUniqueId()` ou hostname
2. **installHash** : SHA1 des 16 premiers caractères du chemin d'installation
3. **instanceSuffix** : Optionnel, pour multi-instance sur même machine

**Exemple de clé** :
```
persistentClientId_MacBookPro_a1b2c3d4e5f6g7h8_dev1
```

#### Caractéristiques
- ✅ **PERSISTE** entre redémarrages
- ✅ **PERSISTE** entre reconnexions réseau
- ✅ **UNIQUE** par installation (support multi-instance)
- ✅ **SURVIT** aux coupures réseau
- ❌ **NE CHANGE JAMAIS** (sauf réinstallation complète)

#### Usage
**Dans le protocole** :
- Envoyé dans le message `register` au serveur
- Utilisé comme identifiant principal dans `client_list`
- Clé pour toutes les associations côté serveur

**Côté client** :
```cpp
m_webSocketClient->setPersistentClientId(persistentClientId);
```

**Côté serveur** :
```javascript
client.persistentId = persistentId; // Stocké dans l'objet client
this.registerSessionForPersistent(persistentId, sessionId);
```

---

### 2.2 Session ID

**Rôle** : Identifiant **ÉPHÉMÈRE** d'une connexion WebSocket spécifique.

#### Génération
**Localisation** : `WebSocketClient.cpp` ligne 16

```cpp
m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
// Nouvelle valeur à CHAQUE lancement du client
```

**Côté serveur** : `server.js` ligne 70
```javascript
const clientId = uuidv4(); // Nouveau UUID par connexion socket
```

#### Caractéristiques
- ✅ **NOUVEAU** à chaque démarrage de l'application
- ✅ **NOUVEAU** à chaque reconnexion WebSocket
- ✅ **UNIQUE** par socket WebSocket
- ❌ **NE PERSISTE PAS** entre redémarrages
- ❌ **CHANGE** à chaque reconnexion

#### Usage Principal
**Client → Serveur** :
```cpp
message["sessionId"] = m_sessionId; // Dans register()
```

**Serveur - Fusion de sessions** :
```javascript
// Si même persistentId ET même sessionId détectés
if (existingSession && existingSession !== client) {
    // Fusion : nouvelle socket remplace l'ancienne
    console.log(`🔄 Client session reconnecting: ${persistentId}`);
    // L'ancienne socket est fermée, la nouvelle hérite de l'état
}
```

#### Cas de Reconnexion

**Scénario 1 : Redémarrage complet du client**
```
1. Client démarre → Nouveau sessionId généré (ex: "abc123")
2. Client se connecte → Envoie persistentId + sessionId
3. Serveur détecte : même persistentId, NOUVEAU sessionId
4. Serveur fusionne : récupère l'état de l'ancien sessionId
```

**Scénario 2 : Perte réseau temporaire**
```
1. Client perd connexion (WiFi coupé)
2. Client se reconnecte → MÊME sessionId (pas de redémarrage)
3. Serveur détecte : même persistentId + même sessionId
4. Serveur restaure la session exacte
```

---

### 2.3 Différence Critique : Persistent vs Session

| Aspect | Persistent Client ID | Session ID |
|--------|---------------------|-----------|
| **Durée de vie** | Permanente (sauf réinstall) | Temporaire (par lancement) |
| **Génération** | Une fois, au premier lancement | À chaque démarrage client |
| **Stockage** | QSettings (disque) | Mémoire uniquement |
| **Usage** | Identité logique stable | Tracking connexion réseau |
| **Reconnexion** | Toujours le même | Nouveau à chaque reboot |
| **Multi-instance** | Un par installation | Un par processus |

**Analogie** :
- **Persistent Client ID** = Numéro de sécurité sociale (permanent)
- **Session ID** = Numéro de ticket de caisse (éphémère)

---

## 3. IDENTIFICATION DES MÉDIAS

### 3.1 Media ID

**Rôle** : Identifiant **UNIQUE** d'un élément graphique sur le canvas.

#### Génération
**Localisation** : `MediaItems.cpp` (constructeur)

```cpp
ResizableMediaBase::ResizableMediaBase(...)
    : m_mediaId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    // Chaque item créé sur le canvas a un UUID unique
}
```

#### Caractéristiques
- ✅ **UNIQUE** par item graphique sur le canvas
- ✅ **PERSISTE** tant que l'item existe dans la scène
- ✅ **DIFFÉRENT** pour chaque copie du même fichier
- ❌ **DISPARAÎT** quand l'item est supprimé
- ❌ **PAS PARTAGÉ** entre clients (local au canvas)

#### Exemple Concret

**Scénario** : Vous ajoutez 3 fois la même image `chat.jpg` sur le canvas

```
Canvas contient :
  - Item 1: mediaId="aaa111" → chat.jpg
  - Item 2: mediaId="bbb222" → chat.jpg  (même fichier, autre item)
  - Item 3: mediaId="ccc333" → chat.jpg  (même fichier, autre item)
```

**Chaque item est indépendant** :
- Position différente
- Taille différente
- Opacité différente
- État d'upload différent

#### Usage

**Association media → file** :
```cpp
FileManager::associateMediaWithFile(mediaId, fileId);
// mediaId "aaa111" → fileId "xyz789" (hash de chat.jpg)
// mediaId "bbb222" → fileId "xyz789" (MÊME fileId !)
// mediaId "ccc333" → fileId "xyz789" (MÊME fileId !)
```

**Tracking UI** :
```cpp
media->setUploadUploading(50); // Progress bar sur cet item spécifique
```

**IMPORTANT** : Le `mediaId` n'est **JAMAIS** envoyé au serveur ou aux autres clients. C'est un identifiant purement local.

---

## 4. IDENTIFICATION DES FICHIERS

### 4.1 File ID

**Rôle** : Identifiant **UNIQUE et DÉTERMINISTE** d'un fichier physique sur le disque.

#### Génération
**Localisation** : `LocalFileRepository.cpp` ligne 10

```cpp
QString LocalFileRepository::generateFileId(const QString& filePath) const {
    QFileInfo fileInfo(filePath);
    QString canonicalPath = fileInfo.canonicalFilePath();
    
    QByteArray pathBytes = canonicalPath.toUtf8();
    QByteArray hash = QCryptographicHash::hash(pathBytes, QCryptographicHash::Sha256);
    
    return QString::fromLatin1(hash.toHex());
    // Exemple : "4f8a9c2d1b3e5f6g7h8i9j0k1l2m3n4o5p6q7r8s9t0u1v2w3x4y5z6"
}
```

#### Algorithme de Génération

**Étape 1** : Résolution du chemin canonique
```cpp
QString canonicalPath = fileInfo.canonicalFilePath();
// /Users/max/Pictures/chat.jpg → /Users/max/Pictures/chat.jpg
// (résout les liens symboliques, relatifs, etc.)
```

**Étape 2** : Hash SHA-256 du chemin
```cpp
QByteArray hash = QCryptographicHash::hash(pathBytes, QCryptographicHash::Sha256);
// Produit un hash de 64 caractères hexadécimaux
```

#### Caractéristiques CRITIQUES

✅ **DÉTERMINISTE** : Même fichier → TOUJOURS même fileId
```cpp
getOrCreateFileId("/path/to/chat.jpg"); // → "4f8a9c2d..."
getOrCreateFileId("/path/to/chat.jpg"); // → "4f8a9c2d..." (MÊME)
```

✅ **DÉDUPLICATION AUTOMATIQUE**
```
Canvas :
  - Item 1 (mediaId aaa111) → /path/chat.jpg → fileId xyz789
  - Item 2 (mediaId bbb222) → /path/chat.jpg → fileId xyz789 (MÊME)
  - Item 3 (mediaId ccc333) → /path/autre.jpg → fileId def456 (DIFFÉRENT)

Upload : Seulement 2 fichiers envoyés (xyz789 et def456), pas 3 !
```

✅ **PARTAGÉ ENTRE MÉDIAS** : Plusieurs `mediaId` peuvent pointer vers le même `fileId`

❌ **DÉPEND DU CHEMIN** : Si le fichier est déplacé, nouveau fileId généré

#### Usage Principal

**Déduplication upload** :
```cpp
// BuildMediaManifest() dans MainWindow.cpp
QSet<QString> processedFileIds; // Évite les doublons

for (auto* media : allMediaItems) {
    QString fileId = fileManager->getFileIdForMedia(media->mediaId());
    if (!processedFileIds.contains(fileId)) {
        files.append({fileId, media->mediaId(), path, ...});
        processedFileIds.insert(fileId);
    }
    // Les doublons sont automatiquement ignorés !
}
```

**Tracking upload côté serveur** :
```javascript
// server.js - handleUploadComplete
const persistentId = this.getPersistentId(senderId);
let filesByIdea = this.clientFiles.get(persistentId);
if (!filesByIdea) {
    filesByIdea = new Map();
    this.clientFiles.set(persistentId, filesByIdea);
}
let fileSet = filesByIdea.get(ideaId);
if (!fileSet) {
    fileSet = new Set();
    filesByIdea.set(ideaId, fileSet);
}
for (const fileId of upload.files) {
    fileSet.add(fileId); // Tracking : ce client a ce fichier pour cette idée
}
```

---

## 5. IDENTIFICATION DES SESSIONS (Canvas)

### 5.1 Canvas Session

**Rôle** : Conteneur d'état pour un client distant spécifique (un canvas par client distant).

#### Structure
**Localisation** : `SessionManager.h`

```cpp
struct CanvasSession {
    QString persistentClientId;  // Client cible (STABLE)
    QString serverAssignedId;    // Session serveur (ÉPHÉMÈRE)
    QString ideaId;              // Scène active (voir section 6)
    ScreenCanvas* canvas;        // Widget Qt du canvas
    QPushButton* uploadButton;   // Bouton d'upload associé
    ClientInfo lastClientInfo;   // Dernier état connu du client
    QSet<QString> expectedIdeaFileIds;   // Fichiers attendus sur ce canvas
    QSet<QString> knownRemoteFileIds;    // Fichiers confirmés sur le client distant
    UploadTracking upload;       // État upload/progress
};
```

#### Gestion
**Par SessionManager** :
```cpp
// MainWindow.cpp
CanvasSession& session = m_sessionManager->getOrCreateSession(persistentClientId, clientInfo);
```

**Lookup par persistentClientId** :
```cpp
CanvasSession* session = m_sessionManager->findSession(persistentClientId);
```

**Lookup par ideaId** :
```cpp
CanvasSession* session = m_sessionManager->findSessionByIdeaId(ideaId);
```

**Lookup par serverSessionId** :
```cpp
CanvasSession* session = m_sessionManager->findSessionByServerClientId(serverSessionId);
```

#### Cycle de Vie

**Création** : Quand un client distant apparaît dans `client_list`
```cpp
ensureCanvasSession(clientInfo);
```

**Persistance** : Tant que le client distant est connu (même déconnecté)

**Suppression** : Uniquement sur demande explicite ou cleanup app

---

## 6. IDENTIFICATION DES IDÉES (SCÈNES)

### 6.1 Idea ID

**Rôle** : Identifiant d'un **projet/scène logique** associé à un canvas spécifique.

#### Génération
**Localisation** : `MainWindow.cpp` ligne 1741

```cpp
QString MainWindow::createIdeaId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Exemple : "9z8y7x6w-5v4u-3t2s-1r0q-p9o8n7m6l5k4"
}
```

#### Génération Automatique
**Quand ?** :
1. Création d'une nouvelle `CanvasSession`
2. Rotation d'idée (après clear canvas)

```cpp
void MainWindow::rotateSessionIdea(CanvasSession& session) {
    const QString oldIdeaId = session.ideaId;
    session.ideaId = createIdeaId(); // NOUVEAU UUID
    session.expectedIdeaFileIds.clear();
    session.knownRemoteFileIds.clear();
    m_fileManager->removeIdeaAssociations(oldIdeaId);
}
```

#### Caractéristiques

✅ **UN PAR CANVAS** : Chaque canvas a son propre ideaId unique
✅ **ASSOCIE FICHIERS** : Tous les fichiers sur un canvas partagent le même ideaId
✅ **OBLIGATOIRE** : Toutes les opérations serveur nécessitent un ideaId (Phase 3)
✅ **ROTATION** : Nouveau ideaId quand on efface le canvas pour repartir à zéro

#### Usage Serveur

**Tracking fichiers par idée** :
```javascript
// Structure serveur
this.clientFiles = new Map(); 
// persistentClientId → Map(ideaId → Set(fileId))

// Exemple :
clientFiles.get("client-abc123") = {
    "idea-001": Set(["file-xyz", "file-def"]),  // Canvas 1
    "idea-002": Set(["file-ghi"])               // Canvas 2 (autre session)
}
```

**Protocole** :
```javascript
// Tous ces messages DOIVENT contenir ideaId
upload_start      → ideaId (obligatoire)
upload_complete   → ideaId (obligatoire)
remove_all_files  → ideaId (obligatoire)
remove_file       → ideaId (obligatoire)
```

#### Exemple Concret

**Scénario** : Vous travaillez avec 2 clients distants

```
Client A (persistentId: "aaa111")
  ├─ Canvas A1 (ideaId: "idea-001")
  │    ├─ media1 → fileId "xyz789"
  │    └─ media2 → fileId "abc456"
  └─ (pas d'autre canvas pour ce client)

Client B (persistentId: "bbb222")
  ├─ Canvas B1 (ideaId: "idea-002")
  │    └─ media3 → fileId "def789"
  └─ (pas d'autre canvas pour ce client)
```

**Côté serveur** :
```javascript
clientFiles = {
    "aaa111": {
        "idea-001": Set(["xyz789", "abc456"])
    },
    "bbb222": {
        "idea-002": Set(["def789"])
    }
}
```

---

## 7. FLUX DE CONNEXION DÉTAILLÉ

### 7.1 Démarrage Client (Première Fois)

```
┌────────────────────────────────────────────────┐
│ 1. Application démarre                         │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 2. MainWindow::MainWindow()                    │
│    - Lit QSettings pour persistentClientId     │
│    - PAS TROUVÉ (première fois)                │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 3. Génération Persistent Client ID             │
│    QString persistentClientId =                │
│      QUuid::createUuid().toString()            │
│    → "3f8c7d2a-5b1e-4f6d-9a2c-1b3d4e5f6g7h"   │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 4. Stockage QSettings                          │
│    settings.setValue(settingsKey,              │
│                      persistentClientId)       │
│    → Sauvegardé sur disque                     │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 5. Configuration WebSocketClient               │
│    m_webSocketClient->setPersistentClientId(   │
│        persistentClientId)                     │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 6. WebSocketClient::WebSocketClient()          │
│    - Génère sessionId (nouveau UUID)           │
│    m_sessionId = QUuid::createUuid()           │
│    → "abc123-def456-ghi789-jkl012"            │
│    - m_clientId = m_sessionId (init)           │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 7. connectToServer()                           │
│    m_webSocket->open(serverUrl)                │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 8. Serveur : nouvelle connexion WebSocket     │
│    const clientId = uuidv4()                   │
│    → "server-socket-123"                       │
│    clientInfo = {                              │
│      id: "server-socket-123",                  │
│      sessionId: "server-socket-123",           │
│      persistentId: null,  ← PAS ENCORE CONNU   │
│      ws: [WebSocket object]                    │
│    }                                           │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 9. Serveur envoie welcome                     │
│    {                                           │
│      type: "welcome",                          │
│      clientId: "server-socket-123"             │
│    }                                           │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 10. Client reçoit welcome                     │
│     m_clientId = "server-socket-123"           │
│     ← Remplace le sessionId temporaire         │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 11. Client envoie register                    │
│     {                                          │
│       type: "register",                        │
│       clientId: "3f8c7d2a-...",  ← PERSISTENT  │
│       sessionId: "abc123-...",   ← SESSION     │
│       machineName: "MacBook Pro",              │
│       screens: [...],                          │
│       platform: "macOS"                        │
│     }                                          │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 12. Serveur handleRegister()                  │
│     client.persistentId = "3f8c7d2a-..."      │
│     client.sessionId = "abc123-..."            │
│     registerSessionForPersistent(              │
│       "3f8c7d2a-...", "abc123-...")           │
│                                                │
│     sessionsByPersistent = {                   │
│       "3f8c7d2a-...": Set(["abc123-..."])     │
│     }                                          │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 13. Serveur envoie registration_confirmed     │
│     + state_sync (si fichiers existants)      │
│     + broadcastClientList()                    │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 14. CLIENT CONNECTÉ ET ENREGISTRÉ ✅          │
└────────────────────────────────────────────────┘
```

---

### 7.2 Redémarrage Client (Déjà Enregistré)

```
┌────────────────────────────────────────────────┐
│ 1. Application redémarre                       │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 2. MainWindow::MainWindow()                    │
│    - Lit QSettings pour persistentClientId     │
│    - TROUVÉ : "3f8c7d2a-..."  ✅               │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 3. Réutilisation Persistent Client ID          │
│    persistentClientId = "3f8c7d2a-..."         │
│    ← MÊME QUE LA DERNIÈRE FOIS                 │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 4. Configuration WebSocketClient               │
│    m_webSocketClient->setPersistentClientId(   │
│        "3f8c7d2a-...")                         │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 5. WebSocketClient::WebSocketClient()          │
│    - Génère NOUVEAU sessionId                  │
│    m_sessionId = QUuid::createUuid()           │
│    → "zzz999-yyy888-xxx777"  ← DIFFÉRENT !     │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 6-10. Connexion identique (welcome, etc.)     │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 11. Client envoie register                    │
│     {                                          │
│       clientId: "3f8c7d2a-...",  ← MÊME !      │
│       sessionId: "zzz999-...",   ← NOUVEAU !   │
│       ...                                      │
│     }                                          │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 12. Serveur handleRegister()                  │
│     - Détecte MÊME persistentId                │
│     - Détecte NOUVEAU sessionId                │
│     - Fusionne avec ancienne session           │
│       (récupère machineId, screens, etc.)      │
│     - Ferme ancienne socket                    │
│     - Nouvelle socket devient active           │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 13. Serveur envoie state_sync                 │
│     {                                          │
│       type: "state_sync",                      │
│       ideas: {                                 │
│         "idea-001": ["file-xyz", "file-abc"],  │
│         "idea-002": ["file-def"]               │
│       }                                        │
│     }                                          │
│     ← Restaure tous les fichiers uploadés     │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 14. CLIENT RECONNECTÉ AVEC ÉTAT RESTAURÉ ✅   │
└────────────────────────────────────────────────┘
```

**Points Clés** :
- ✅ `persistentClientId` : **IDENTIQUE** (récupéré depuis QSettings)
- 🔄 `sessionId` : **NOUVEAU** (généré à chaque démarrage)
- 🔄 Socket WebSocket : **NOUVELLE** connexion
- ✅ État serveur : **RESTAURÉ** automatiquement

---

### 7.3 Reconnexion Réseau (Même Session)

```
┌────────────────────────────────────────────────┐
│ 1. Client connecté et actif                   │
│    persistentId: "3f8c7d2a-..."               │
│    sessionId: "abc123-..."                    │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 2. Perte réseau (WiFi coupé, etc.)           │
│    WebSocket: disconnected                     │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 3. Client détecte perte connexion             │
│    onDisconnected()                            │
│    - m_reconnectTimer démarre                  │
│    - Tentatives de reconnexion                 │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 4. Réseau revient                             │
│    attemptReconnect()                          │
│    m_webSocket->open(serverUrl)                │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 5. Nouvelle socket WebSocket ouverte          │
│    ← MAIS : sessionId INCHANGÉ (pas reboot)   │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 6. Serveur : nouvelle connexion               │
│    Nouveau socket ID assigné                   │
│    → "server-socket-456"  ← NOUVEAU            │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 7. Client envoie register                     │
│    {                                           │
│      clientId: "3f8c7d2a-...",  ← MÊME         │
│      sessionId: "abc123-...",   ← MÊME !       │
│      ...                                       │
│    }                                           │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 8. Serveur handleRegister()                   │
│    - Détecte : MÊME persistentId + sessionId   │
│    - Trouve session existante (déconnectée)    │
│    - Réassigne nouvelle socket                 │
│    - RESTAURE état complet                     │
│    existingSession = clients.get("abc123-...") │
│    ← Session retrouvée !                       │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 9. Serveur envoie state_sync                  │
│    Tous les fichiers/idées restaurés           │
└────────────────┬───────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────┐
│ 10. CLIENT RECONNECTÉ SEAMLESSLY ✅           │
│     Aucune perte d'état                        │
└────────────────────────────────────────────────┘
```

**Points Clés** :
- ✅ `persistentClientId` : **IDENTIQUE**
- ✅ `sessionId` : **IDENTIQUE** (pas de reboot)
- 🔄 Socket : **NOUVELLE** (reconnexion réseau)
- ✅ État : **100% RESTAURÉ** (seamless)

---

## 8. DIAGRAMME COMPLET

### 8.1 Relations Entre Tous les IDs

```
┌──────────────────────────────────────────────────────────────┐
│                    CLIENT APPLICATION                         │
├──────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌─────────────────────────────────────────────────┐         │
│  │ Persistent Client ID (QSettings)                │         │
│  │ "3f8c7d2a-5b1e-4f6d-9a2c-1b3d4e5f6g7h"         │         │
│  │ ↓ STABLE - survit aux redémarrages              │         │
│  └────────────────────┬────────────────────────────┘         │
│                       │                                       │
│                       │ setPersistentClientId()               │
│                       ▼                                       │
│  ┌─────────────────────────────────────────────────┐         │
│  │ WebSocketClient                                 │         │
│  │  ├─ m_persistentClientId (stocké)               │         │
│  │  ├─ m_sessionId (généré au démarrage)           │         │
│  │  │   "abc123-def456-ghi789-jkl012"              │         │
│  │  │   ↑ ÉPHÉMÈRE - change à chaque lancement     │         │
│  │  └─ m_clientId (reçu du serveur)                │         │
│  └─────────────────────────────────────────────────┘         │
│                       │                                       │
│                       │ register(persistentId, sessionId)     │
│                       ▼                                       │
└──────────────────────┼───────────────────────────────────────┘
                       │
                       │ WebSocket
                       │
┌──────────────────────┼───────────────────────────────────────┐
│                      ▼                   SERVER               │
├──────────────────────────────────────────────────────────────┤
│  ┌──────────────────────────────────────────────┐            │
│  │ clients Map<sessionId, ClientInfo>           │            │
│  │   "abc123-..." → {                            │            │
│  │     id: "abc123-...",                         │            │
│  │     sessionId: "abc123-...",                  │            │
│  │     persistentId: "3f8c7d2a-...", ← LIÉ       │            │
│  │     ws: [WebSocket],                          │            │
│  │     machineName: "MacBook Pro"                │            │
│  │   }                                           │            │
│  └──────────────────────────────────────────────┘            │
│                                                               │
│  ┌──────────────────────────────────────────────┐            │
│  │ sessionsByPersistent Map                      │            │
│  │   "3f8c7d2a-..." → Set(["abc123-..."])       │            │
│  │   ↑ Permet de retrouver toutes les sessions  │            │
│  │     d'un même client persistant               │            │
│  └──────────────────────────────────────────────┘            │
│                                                               │
│  ┌──────────────────────────────────────────────┐            │
│  │ clientFiles Map (tracking fichiers)          │            │
│  │   "3f8c7d2a-..." → Map(                      │            │
│  │     "idea-001" → Set(["file-xyz", "file-abc"]) │          │
│  │     "idea-002" → Set(["file-def"])            │            │
│  │   )                                           │            │
│  └──────────────────────────────────────────────┘            │
│                                                               │
└───────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│                    CLIENT - CANVAS SIDE                       │
├──────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌──────────────────────────────────────────────┐            │
│  │ SessionManager                                │            │
│  │   Map<persistentClientId, CanvasSession>      │            │
│  │                                                │            │
│  │   "3f8c7d2a-..." → CanvasSession {            │            │
│  │     persistentClientId: "3f8c7d2a-...",       │            │
│  │     ideaId: "idea-001",  ← Une idée par canvas│            │
│  │     canvas: [ScreenCanvas widget],            │            │
│  │     expectedIdeaFileIds: Set([...]),          │            │
│  │     knownRemoteFileIds: Set([...])            │            │
│  │   }                                           │            │
│  └──────────────────────────────────────────────┘            │
│                       │                                       │
│                       │ canvas contient                       │
│                       ▼                                       │
│  ┌──────────────────────────────────────────────┐            │
│  │ Media Items (QGraphicsScene)                 │            │
│  │                                               │            │
│  │   ResizableMediaBase {                       │            │
│  │     m_mediaId: "aaa111"  ← UUID unique item  │            │
│  │     m_fileId: "xyz789"   ← Hash chemin       │            │
│  │     m_sourcePath: "/path/to/chat.jpg"        │            │
│  │   }                                           │            │
│  │                                               │            │
│  │   ResizableMediaBase {                       │            │
│  │     m_mediaId: "bbb222"  ← UUID différent    │            │
│  │     m_fileId: "xyz789"   ← MÊME fileId !     │            │
│  │     m_sourcePath: "/path/to/chat.jpg"        │            │
│  │   }                                           │            │
│  └──────────────────────────────────────────────┘            │
│                       │                                       │
│                       │ lookup via FileManager                │
│                       ▼                                       │
│  ┌──────────────────────────────────────────────┐            │
│  │ FileManager                                   │            │
│  │   LocalFileRepository:                        │            │
│  │     Map<fileId, path>                         │            │
│  │     "xyz789" → "/path/to/chat.jpg"            │            │
│  │                                               │            │
│  │   RemoteFileTracker:                          │            │
│  │     Map<fileId, Set<clientIds>>               │            │
│  │     "xyz789" → Set(["3f8c7d2a-..."])         │            │
│  │                                               │            │
│  │   Associations:                               │            │
│  │     Map<mediaId, fileId>                      │            │
│  │     "aaa111" → "xyz789"                       │            │
│  │     "bbb222" → "xyz789"                       │            │
│  │                                               │            │
│  │     Map<fileId, Set<ideaIds>>                 │            │
│  │     "xyz789" → Set(["idea-001", "idea-002"]) │            │
│  └──────────────────────────────────────────────┘            │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

---

### 8.2 Flux Upload Complet avec Tous les IDs

```
┌─────────────────────────────────────────────────────────────┐
│ PRÉPARATION UPLOAD                                           │
└─────────────────────────────────────────────────────────────┘

Client A (sender)                                  Server
persistentId: "aaa111"
sessionId: "session-A"
                                                   
Canvas pour Client B (target):
persistentId: "bbb222"  ← Target
ideaId: "idea-001"

Items sur canvas:
  - media1: mediaId "mmm111" → fileId "fff777" (chat.jpg)
  - media2: mediaId "mmm222" → fileId "fff888" (dog.jpg)
  - media3: mediaId "mmm333" → fileId "fff777" (chat.jpg, doublon)

┌─────────────────────────────────────────────────────────────┐
│ ÉTAPE 1 : Build Manifest (déduplication)                    │
└─────────────────────────────────────────────────────────────┘

processedFileIds = Set()

Pour chaque media:
  ✓ media1: fileId "fff777" → Ajouter (premier)
  ✗ media2: fileId "fff888" → Ajouter
  ✗ media3: fileId "fff777" → SKIP (déjà dans processedFileIds)

Résultat : 2 fichiers à uploader (pas 3 !)

manifest = [
  {
    fileId: "fff777",
    mediaId: "mmm111",  ← Premier media trouvé avec ce fichier
    name: "chat.jpg",
    size: 1024000
  },
  {
    fileId: "fff888",
    mediaId: "mmm222",
    name: "dog.jpg",
    size: 2048000
  }
]

┌─────────────────────────────────────────────────────────────┐
│ ÉTAPE 2 : Upload Start                                      │
└─────────────────────────────────────────────────────────────┘

Client A → Server:
{
  type: "upload_start",
  targetClientId: "bbb222",  ← persistentId du target
  uploadId: "upload-xyz123",  ← UUID unique de cet upload
  ideaId: "idea-001",         ← Obligatoire !
  filesManifest: [...]
}

Server reçoit:
- Crée tracking: uploads.set("upload-xyz123", {
    sender: "aaa111",
    target: "bbb222",
    ideaId: "idea-001",
    files: ["fff777", "fff888"],
    startTime: Date.now()
  })

Server → Client B:
{
  type: "upload_start",
  senderClientId: "aaa111",
  uploadId: "upload-xyz123",
  ideaId: "idea-001",
  filesManifest: [...]
}

Client B:
- Crée dossier cache pour uploadId
- Prépare fichiers vides pour chaque fileId

┌─────────────────────────────────────────────────────────────┐
│ ÉTAPE 3 : Upload Chunks                                     │
└─────────────────────────────────────────────────────────────┘

Pour chaque fichier, pour chaque chunk:

Client A → Server:
{
  type: "upload_chunk",
  targetClientId: "bbb222",
  uploadId: "upload-xyz123",
  fileId: "fff777",      ← Identifie le fichier
  chunkIndex: 0,
  data: "base64...",
  ideaId: "idea-001"
}

Server → Client B: (relay direct)
{
  type: "upload_chunk",
  senderClientId: "aaa111",
  uploadId: "upload-xyz123",
  fileId: "fff777",
  chunkIndex: 0,
  data: "base64...",
  ideaId: "idea-001"
}

Client B:
- Écrit chunk dans fichier
- Calcule progress
- Envoie feedback au sender

Client B → Server:
{
  type: "upload_progress",
  senderClientId: "aaa111",
  uploadId: "upload-xyz123",
  percent: 45,
  filesCompleted: 1,
  totalFiles: 2,
  completedFileIds: ["fff777"],  ← Premier fichier terminé
  perFileProgress: {
    "fff777": 100,
    "fff888": 0
  }
}

Server → Client A: (relay)
{...même message...}

Client A reçoit:
- Met à jour UI globale: "Uploading (1/2) 45%"
- Met à jour items individuels:
    * media1 (fileId fff777): 100% ✅
    * media2 (fileId fff888): 0%
    * media3 (fileId fff777): 100% ✅ (même fichier que media1!)

┌─────────────────────────────────────────────────────────────┐
│ ÉTAPE 4 : Upload Complete                                   │
└─────────────────────────────────────────────────────────────┘

Client A → Server:
{
  type: "upload_complete",
  targetClientId: "bbb222",
  uploadId: "upload-xyz123",
  ideaId: "idea-001"
}

Server:
- Récupère upload: uploads.get("upload-xyz123")
- Met à jour clientFiles:
  
  clientFiles.get("bbb222").get("idea-001") = Set([
    "fff777",  ← Ajouté
    "fff888"   ← Ajouté
  ])

- Supprime tracking upload temporaire
- Relay au target

Server → Client B:
{
  type: "upload_complete",
  senderClientId: "aaa111",
  uploadId: "upload-xyz123",
  ideaId: "idea-001"
}

Client B:
- Vérifie tous les fichiers reçus
- Envoie confirmation finale

Client B → Server:
{
  type: "upload_finished",
  senderClientId: "aaa111",
  uploadId: "upload-xyz123"
}

Server → Client A: (relay)
{...même message...}

Client A:
- Marque tous les items comme "Uploaded"
- media1: État = Uploaded ✅
- media2: État = Uploaded ✅
- media3: État = Uploaded ✅ (même fileId que media1)

┌─────────────────────────────────────────────────────────────┐
│ RÉSULTAT FINAL                                              │
└─────────────────────────────────────────────────────────────┘

Client A (sender):
  FileManager associations:
    - fileId "fff777" → clients: ["bbb222"]
    - fileId "fff888" → clients: ["bbb222"]
    - fileId "fff777" → ideaIds: ["idea-001"]
    - fileId "fff888" → ideaIds: ["idea-001"]

Server tracking:
  clientFiles.get("bbb222") = {
    "idea-001": Set(["fff777", "fff888"])
  }

Client B (target):
  Cache files:
    - /cache/upload-xyz123/fff777 (chat.jpg)
    - /cache/upload-xyz123/fff888 (dog.jpg)
  
  FileManager:
    - Registered: fff777 → /cache/.../fff777
    - Registered: fff888 → /cache/.../fff888

✅ 2 fichiers physiques uploadés
✅ 3 media items marqués comme uploaded
✅ Déduplication réussie (chat.jpg uploadé 1 fois, pas 2)
```

---

## 9. CAS D'USAGE CRITIQUES

### 9.1 Multi-Instance sur Même Machine

**Question** : Comment gérer 2 instances de Mouffette sur le même Mac ?

**Réponse** :

```bash
# Terminal 1 - Instance principale (default)
$ ./MouffetteClient.app/Contents/MacOS/MouffetteClient

# Terminal 2 - Instance "dev"
$ MOUFFETTE_INSTANCE_SUFFIX=dev1 ./MouffetteClient.app/Contents/MacOS/MouffetteClient

# Terminal 3 - Instance "test"
$ ./MouffetteClient.app/Contents/MacOS/MouffetteClient --instance-suffix=test1
```

**Résultat** :
```
Instance 1:
  persistentId: "uuid-aaa111" (clé: persistentClientId_MacBookPro_a1b2c3d4)
  sessionId: "session-111"

Instance 2:
  persistentId: "uuid-bbb222" (clé: persistentClientId_MacBookPro_a1b2c3d4_dev1)
  sessionId: "session-222"

Instance 3:
  persistentId: "uuid-ccc333" (clé: persistentClientId_MacBookPro_a1b2c3d4_test1)
  sessionId: "session-333"
```

**Serveur voit** : 3 clients complètement distincts et indépendants.

---

### 9.2 Récupération Après Crash

**Scénario** : Client crash pendant un upload

**État Avant Crash** :
```
Server clientFiles:
  "client-aaa111": {
    "idea-001": Set(["file-xyz", "file-abc"])  ← Fichiers présents
  }
```

**Que se passe-t-il ?**

1. **Client redemarre**
   - Même `persistentId` (récupéré depuis QSettings)
   - Nouveau `sessionId` (généré au boot)

2. **Reconnexion**
   ```
   Client → Server: register(persistentId, nouveau sessionId)
   ```

3. **Server detecte le client connu**
   ```javascript
   const persistentId = message.clientId; // "client-aaa111"
   // Trouve les fichiers existants
   const ideas = this.clientFiles.get(persistentId);
   ```

4. **Server envoie state_sync**
   ```json
   {
     "type": "state_sync",
     "ideas": {
       "idea-001": ["file-xyz", "file-abc"]
     }
   }
   ```

5. **Client restaure l'état**
   ```cpp
   // MainWindow::handleStateSyncFromServer()
   for (const QString& fileId : ideaFileIds) {
       session->knownRemoteFileIds.insert(fileId);
   }
   // Tous les items marqués comme "Uploaded" ✅
   ```

**Résultat** : Aucune perte de données, état 100% restauré.

---

### 9.3 Upload Même Fichier Vers 2 Clients

**Scénario** : Vous uploadez `chat.jpg` vers Client B et Client C

**Canvas A (vers Client B)** :
```
ideaId: "idea-001"
media1: mediaId "mmm111" → fileId "fff777" (chat.jpg)
```

**Canvas B (vers Client C)** :
```
ideaId: "idea-002"
media2: mediaId "mmm222" → fileId "fff777" (chat.jpg, MÊME fileId !)
```

**Upload 1 (vers Client B)** :
```
upload_start → targetClientId: "bbb222", ideaId: "idea-001", fileId: "fff777"
...chunks...
upload_complete

Server:
  clientFiles.get("bbb222").get("idea-001") = Set(["fff777"])
```

**Upload 2 (vers Client C)** :
```
upload_start → targetClientId: "ccc333", ideaId: "idea-002", fileId: "fff777"
...chunks...
upload_complete

Server:
  clientFiles.get("ccc333").get("idea-002") = Set(["fff777"])
```

**FileManager (Client A)** :
```cpp
RemoteFileTracker:
  fileId "fff777" → clients: Set(["bbb222", "ccc333"])

IdeaAssociations:
  fileId "fff777" → ideaIds: Set(["idea-001", "idea-002"])
```

**Résultat** :
- ✅ Même fichier uploadé 2 fois (unavoidable, 2 destinations différentes)
- ✅ Tracking correct par client ET par ideaId
- ✅ Si vous supprimez media1, fichier reste pour media2

---

### 9.4 Suppression Fichier Unique Parmi Doublons

**Setup** :
```
Canvas (ideaId "idea-001"):
  media1: mediaId "mmm111" → fileId "fff777" (chat.jpg, position A)
  media2: mediaId "mmm222" → fileId "fff777" (chat.jpg, position B, doublon)
  media3: mediaId "mmm333" → fileId "fff888" (dog.jpg)
```

**Action** : Supprimer media1 (premier chat.jpg)

**Code** :
```cpp
// MediaSettingsPanel - Delete button clicked
FileManager::removeMediaAssociation(mediaId);
```

**Dans FileManager** :
```cpp
void FileManager::removeMediaAssociation(const QString& mediaId) {
    QString fileId = m_mediaIdToFileId.value(mediaId);
    
    // Supprime l'association media → file
    m_mediaIdToFileId.remove(mediaId);
    
    // Supprime de la liste file → medias
    QList<QString>& mediaIds = m_fileIdToMediaIds[fileId];
    mediaIds.removeAll(mediaId);
    
    // Si plus aucun media ne référence ce fichier
    if (mediaIds.isEmpty()) {
        m_fileIdToMediaIds.remove(fileId);
        removeFileIfUnused(fileId);  ← Cleanup complet
    }
}
```

**Résultat pour ce cas** :
```
Après suppression de media1:
  m_fileIdToMediaIds["fff777"] = ["mmm222"]  ← media2 reste !
  
  → removeFileIfUnused() n'est PAS appelé
  → Fichier fff777 reste dans le système
  → media2 continue d'afficher chat.jpg ✅
```

**Si on supprime AUSSI media2** :
```
Après suppression de media2:
  m_fileIdToMediaIds["fff777"] = []  ← Plus de références
  
  → removeFileIfUnused() EST appelé
  → Notify serveur pour supprimer chez tous les clients
  → Fichier fff777 complètement supprimé
```

---

## 10. RÉSUMÉ EXÉCUTIF

### Table de Correspondance Rapide

| ID Type | Génération | Durée de Vie | Scope | Exemple |
|---------|-----------|--------------|-------|---------|
| **Persistent Client ID** | 1 fois (QSettings) | Permanent | Installation | `3f8c7d2a-5b1e-4f6d-9a2c` |
| **Session ID** | Chaque boot | Temporaire | Processus | `abc123-def456-ghi789` |
| **Media ID** | Par item canvas | Item lifetime | Canvas local | `mmm111`, `mmm222` |
| **File ID** | SHA256 path | Fichier lifetime | Fichier physique | `fff777` (hash de /path/chat.jpg) |
| **Idea ID** | Par canvas | Canvas lifetime | Canvas/Scène | `idea-001` |

### Questions Fréquentes

**Q1 : Pourquoi 2 IDs client (persistent + session) ?**
- **Persistent** : Identité stable pour tracking à long terme
- **Session** : Tracking connexion réseau temporaire pour gestion reconnexion

**Q2 : Pourquoi media ID ET file ID ?**
- **Media ID** : Item graphique unique sur canvas (position, taille, état)
- **File ID** : Fichier physique unique (déduplication, upload)
- Plusieurs media items peuvent partager le même file (copies sur canvas)

**Q3 : Comment le serveur sait que c'est le même client après reboot ?**
- Le client envoie toujours le même `persistentClientId` (lu depuis QSettings)
- Server fusion automatiquement les sessions basé sur ce persistentId

**Q4 : Que se passe-t-il si je déplace un fichier ?**
- Nouveau `fileId` généré (hash basé sur le chemin)
- Ancien fileId n'est plus valide
- Upload nécessaire si le fichier a été uploadé précédemment

**Q5 : Puis-je avoir plusieurs clients sur la même machine ?**
- Oui, utiliser `MOUFFETTE_INSTANCE_SUFFIX=dev1` ou `--instance-suffix=test1`
- Chaque instance a son propre `persistentClientId` unique

---

## 11. SCHÉMA DE DÉCISION

```
┌────────────────────────────────────────────────────────┐
│ Quel ID utiliser pour...                               │
└────────────────────────────────────────────────────────┘

❓ Identifier un CLIENT de manière stable ?
   → Persistent Client ID
   Exemples : client_list, state_sync, tracking uploads

❓ Tracker une CONNEXION réseau spécifique ?
   → Session ID
   Exemples : fusion sessions, reconnexion seamless

❓ Identifier un ITEM GRAPHIQUE sur le canvas ?
   → Media ID
   Exemples : selection UI, progress bars individuelles

❓ Identifier un FICHIER PHYSIQUE (déduplication) ?
   → File ID
   Exemples : manifeste upload, tracking fichiers uploadés

❓ Identifier une SCÈNE/PROJET sur un canvas ?
   → Idea ID
   Exemples : associations fichiers, state sync par scène

┌────────────────────────────────────────────────────────┐
│ Relations Clés                                          │
└────────────────────────────────────────────────────────┘

1 Persistent Client ID → N Session IDs (dans le temps)
1 Persistent Client ID → N Canvas Sessions (un par client distant)
1 Canvas Session → 1 Idea ID
1 Idea ID → N File IDs (fichiers sur ce canvas)
1 File ID → N Media IDs (items dupliqués sur canvas)
1 Media ID → 1 File ID (mais plusieurs medias → même file OK)
```

---

**FIN DU RAPPORT**

Ce document constitue la référence complète pour comprendre tous les systèmes d'identification dans Mouffette. Pour toute question ou clarification, référez-vous aux sections pertinentes ci-dessus.
