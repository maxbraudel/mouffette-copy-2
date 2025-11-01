# Rapport d'analyse : Suppression inappropriée de fichiers source

## 📋 Résumé exécutif

**Problème identifié** : Lorsqu'un Client A importe un média sur le canvas du Client B, les fichiers source des médias du Client B présents sur le canvas du Client A sont supprimés du disque.

**Sévérité** : 🔴 CRITIQUE - Perte de données utilisateur

**Cause racine** : Confusion entre deux types de fichiers avec des gestions de cycle de vie différentes :
- **Fichiers sources** (importés par l'utilisateur depuis son système de fichiers)
- **Fichiers de cache remote** (reçus d'autres clients, stockés temporairement)

## 🔍 Analyse technique détaillée

### 1. Architecture du système de fichiers

Le système utilise plusieurs composants :

#### LocalFileRepository
- **Rôle** : Gère les mappings bidirectionnels `fileId ↔ filePath`
- **FileId** : Hash SHA-256 du chemin canonique du fichier
- **Mappings** :
  - `m_fileIdToPath` : fileId → chemin absolu
  - `m_pathToFileId` : chemin absolu → fileId

#### RemoteFileTracker
- **Rôle** : Suit la distribution des fichiers entre clients
- **Mappings** :
  - `m_fileIdToClients` : fileId → Set<clientId>
  - `m_fileIdToIdeaIds` : fileId → Set<canvasSessionId>
  - `m_canvasSessionIdToFileIds` : canvasSessionId → Set<fileId>

#### FileManager
- **Rôle** : Orchestre les opérations de fichiers
- **Mappings** :
  - `m_mediaIdToFileId` : mediaId → fileId
  - `m_fileIdToMediaIds` : fileId → List<mediaId>

### 2. Flux de données - Import local (Client A)

```
1. Utilisateur importe image.png depuis /Users/max/Documents/image.png
2. MediaItems::setSourcePath("/Users/max/Documents/image.png")
3. FileManager::getOrCreateFileId() 
   → génère fileId (ex: "abc123...")
4. LocalFileRepository enregistre :
   fileId "abc123" → "/Users/max/Documents/image.png"
5. FileManager::associateMediaWithFile(mediaId, fileId)
6. FileWatcher::watchMediaItem() surveille le fichier source
```

**Type** : FICHIER SOURCE - Ne doit JAMAIS être supprimé par l'application

### 3. Flux de données - Réception remote (Client B reçoit média de A)

```
1. UploadManager reçoit "upload_start" avec fileId "abc123"
2. Crée fichier cache : 
   ~/.cache/Mouffette/Uploads/clientA_id/abc123.png
3. UploadManager::registerReceivedFilePath(fileId, cachePath)
4. LocalFileRepository enregistre :
   fileId "abc123" → "~/.cache/Mouffette/Uploads/clientA_id/abc123.png"
5. RemoteFileTracker associe fileId avec canvasSessionId
```

**Type** : FICHIER CACHE - Peut être supprimé quand non utilisé

### 4. Le problème : Collision de fileId

#### Scénario détaillé

**État initial :**
- Client A a importé `image1.png` (chemin: `/Users/A/image1.png`)
  - fileId: `hash("/Users/A/image1.png")` = `"xyz789"`
  - LocalFileRepository A : `"xyz789" → "/Users/A/image1.png"`
  
- Client B envoie son média `photo.jpg` à Client A
  - Reçu dans cache : `~/.cache/Mouffette/Uploads/clientB/fileId.jpg`
  - fileId: `"abc123"`
  - LocalFileRepository A : `"abc123" → "~/.cache/Mouffette/Uploads/clientB/abc123.jpg"`

**Problème - Cas 1 : Collision de hash (peu probable mais possible)**

Si par hasard :
```
hash("/Users/A/image1.png") == hash("~/.cache/.../photo.jpg")
```

Alors le même fileId représente deux fichiers différents !

**Problème - Cas 2 : registerReceivedFilePath écrase le mapping**

```cpp
void LocalFileRepository::registerReceivedFilePath(...) {
    if (m_fileIdToPath.contains(fileId)) {
        return; // N'écrase PAS - mais le problème existe ailleurs
    }
    m_fileIdToPath.insert(fileId, canonicalPath);
    m_pathToFileId.insert(canonicalPath, fileId);
}
```

⚠️ **PROBLÈME IDENTIFIÉ** : La fonction `registerReceivedFilePath` ne peut PAS distinguer si un fileId existant correspond à :
- Un fichier source local (qu'on doit préserver)
- Un ancien fichier de cache (qu'on peut remplacer)

### 5. Chaîne de suppression fatale

#### Étape 1 : Suppression d'un média remote
```cpp
// MainWindow.cpp ligne 468
connect(m_fileWatcher, &FileWatcher::filesDeleted, this, [this](...) {
    // Quand le fichier source est détecté comme supprimé
    mediaItem->prepareForDeletion();
    scene()->removeItem(mediaItem);
    delete mediaItem;
});
```

#### Étape 2 : Destructeur du MediaItem
```cpp
// MediaItems.cpp ligne 75
ResizableMediaBase::~ResizableMediaBase() {
    if (!m_mediaId.isEmpty() && s_fileManager) {
        s_fileManager->removeMediaAssociation(m_mediaId);
    }
}
```

#### Étape 3 : Nettoyage du FileManager
```cpp
// FileManager.cpp ligne 72
void FileManager::removeMediaAssociation(const QString& mediaId) {
    const QString fileId = m_mediaIdToFileId.value(mediaId);
    m_fileIdToMediaIds[fileId].removeAll(mediaId);
    m_mediaIdToFileId.remove(mediaId);
    
    removeFileIfUnused(fileId); // ← DANGER
}
```

#### Étape 4 : Le piège fatal
```cpp
// FileManager.cpp ligne 114
void FileManager::removeFileIfUnused(const QString& fileId) {
    if (!m_fileIdToMediaIds.contains(fileId)) return;
    if (!m_fileIdToMediaIds.value(fileId).isEmpty()) return;
    
    // Nettoie tout sans distinction !
    m_repository->removeFileMapping(fileId);
    m_tracker->removeAllTrackingForFile(fileId);
    m_cache->releaseFileMemory(fileId);
}
```

#### Étape 5 : Le message "remove_file" destructeur
```cpp
// MainWindow.cpp ligne 447-460 (callback FileRemovalNotifier)
[](const QString& fileId, const QList<QString>& clientIds, const QList<QString>& canvasSessionIds) {
    for (const QString& clientId : clientIds) {
        // Envoie "remove_file" aux autres clients
        m_webSocketClient->sendRemoveFile(clientId, canvasSessionId, fileId);
    }
}
```

#### Étape 6 : Le client remote supprime SON fichier source
```cpp
// UploadManager.cpp ligne 960-972 (réception de "remove_file")
const QString mappedPath = m_fileManager->getFilePathForId(fileId);
if (!mappedPath.isEmpty()) {
    QFile file(mappedPath);
    if (file.remove()) { // ← SUPPRIME LE FICHIER SOURCE DE L'UTILISATEUR
        qDebug() << "Removed mapped cached file" << mappedPath;
    }
}
```

### 6. Scénario de reproduction

1. **Client A** importe `vacation.jpg` depuis `/Users/Alice/Photos/vacation.jpg`
   - fileId généré: `"photo_xyz"`
   - LocalFileRepository A: `"photo_xyz" → "/Users/Alice/Photos/vacation.jpg"`

2. **Client B** importe `meeting.mp4` sur le canvas de A
   - Client A reçoit et stocke dans cache: `~/.cache/Mouffette/Uploads/clientB_id/video_abc.mp4`
   - fileId: `"video_abc"`

3. **Client A** importe `document.pdf` sur le canvas de B
   - Client B reçoit et stocke: `~/.cache/Mouffette/Uploads/clientA_id/doc_xyz.pdf`

4. **Le drame** : Si Client A supprime son média remote (meeting.mp4)
   - `removeMediaAssociation` → `removeFileIfUnused("video_abc")`
   - Envoie `remove_file` à Client B
   - **Client B reçoit** : supprime le fichier `mappedPath` pour `"video_abc"`
   - **Mais** : Si par malchance `"video_abc"` correspond aussi à un fichier source de B...
   - **Résultat** : Le fichier source de B est supprimé du disque !

### 7. Cas edge aggravants

#### Cas A : Même nom de fichier
```
Client A: /Users/Alice/image.png → fileId basé sur chemin complet
Client B: /Users/Bob/image.png → fileId différent normalement
```
Pas de collision car les chemins complets sont différents.

#### Cas B : Collision de hash SHA-256
Probabilité : ~1 / 2^256 (astronomiquement faible)
Mais avec des millions de fichiers, possible en théorie.

#### Cas C : Réutilisation de fileId
Si un fichier est déplacé puis remplacé par un autre au même endroit,
le fileId (basé sur le chemin canonique) peut être réutilisé.

## 🎯 Cause racine identifiée

**Le système ne distingue pas** :

1. **Fichiers sources locaux** (appartenant à l'utilisateur)
   - Chemins : `/Users/xxx/Documents/...`, `/Users/xxx/Pictures/...`
   - Cycle de vie : PERMANENT - Ne doivent JAMAIS être supprimés
   - Propriétaire : Utilisateur local

2. **Fichiers de cache remote** (reçus d'autres clients)
   - Chemins : `~/.cache/Mouffette/Uploads/{clientId}/...`
   - Cycle de vie : TEMPORAIRE - Peuvent être supprimés
   - Propriétaire : Cache de l'application

Le FileManager utilise le même système de fileId pour les deux types,
et `removeFileIfUnused` ne vérifie pas le type de fichier avant suppression.

## 🔧 Solutions recommandées

### Solution 1 : Ajouter un flag "isSourceFile" (RECOMMANDÉ)

```cpp
// LocalFileRepository.h
class LocalFileRepository {
private:
    QHash<QString, QString> m_fileIdToPath;
    QHash<QString, bool> m_fileIdIsSourceFile; // ← NOUVEAU
    
public:
    void registerSourceFile(const QString& fileId, const QString& path) {
        m_fileIdToPath[fileId] = path;
        m_fileIdIsSourceFile[fileId] = true; // Fichier source protégé
    }
    
    void registerCacheFile(const QString& fileId, const QString& path) {
        m_fileIdToPath[fileId] = path;
        m_fileIdIsSourceFile[fileId] = false; // Fichier cache
    }
    
    bool isSourceFile(const QString& fileId) const {
        return m_fileIdIsSourceFile.value(fileId, false);
    }
};
```

```cpp
// UploadManager.cpp - Modification "remove_file"
const QString mappedPath = m_fileManager->getFilePathForId(fileId);
if (!mappedPath.isEmpty()) {
    // PROTECTION : Ne jamais supprimer un fichier source
    if (m_repository->isSourceFile(fileId)) {
        qWarning() << "Refusing to delete source file:" << mappedPath;
        return; // ← PROTECTION
    }
    
    QFile file(mappedPath);
    if (file.remove()) {
        qDebug() << "Removed cached file" << mappedPath;
    }
}
```

### Solution 2 : Vérifier le chemin avant suppression

```cpp
// UploadManager.cpp
const QString mappedPath = m_fileManager->getFilePathForId(fileId);
if (!mappedPath.isEmpty()) {
    // PROTECTION : Vérifier que c'est un fichier de cache
    QString cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!mappedPath.startsWith(cachePath)) {
        qWarning() << "Refusing to delete non-cache file:" << mappedPath;
        return; // ← PROTECTION
    }
    
    QFile file(mappedPath);
    if (file.remove()) {
        qDebug() << "Removed cached file" << mappedPath;
    }
}
```

### Solution 3 : Séparer les fileId sources et cache

```cpp
// Utiliser des préfixes différents
QString sourceFileId = "SOURCE_" + hash(path);
QString cacheFileId = "CACHE_" + hash(path);

// Ou deux repositories distincts
LocalFileRepository sources;
CacheFileRepository cache;
```

## 📊 Recommandation finale

**Solution hybride (1 + 2)** :

1. ✅ Ajouter le flag `isSourceFile` dans LocalFileRepository
2. ✅ Vérifier le préfixe de chemin comme double protection
3. ✅ Modifier `removeFileIfUnused` pour ne JAMAIS supprimer les sources
4. ✅ Ajouter des logs détaillés pour tracer les suppressions

```cpp
void FileManager::removeFileIfUnused(const QString& fileId) {
    if (!m_fileIdToMediaIds.contains(fileId)) return;
    if (!m_fileIdToMediaIds.value(fileId).isEmpty()) return;
    
    // PROTECTION : Vérifier le type de fichier
    QString path = m_repository->getFilePathForId(fileId);
    if (m_repository->isSourceFile(fileId)) {
        qDebug() << "FileManager: Keeping source file" << path << "(no longer in use but protected)";
        // Nettoyer uniquement les associations, pas le fichier
        m_fileIdToMediaIds.remove(fileId);
        m_tracker->removeAllTrackingForFile(fileId);
        m_cache->releaseFileMemory(fileId);
        // NE PAS appeler removeFileMapping pour les sources
        return;
    }
    
    // Pour les fichiers de cache, suppression normale
    qDebug() << "FileManager: Cleaning up cache file" << path;
    m_fileIdToMediaIds.remove(fileId);
    m_repository->removeFileMapping(fileId);
    m_tracker->removeAllTrackingForFile(fileId);
    m_cache->releaseFileMemory(fileId);
}
```

## 🧪 Tests recommandés

1. **Test de base** : Importer un fichier local, vérifier qu'il existe toujours après suppression du média
2. **Test cross-client** : Client A envoie média à B, B le supprime, vérifier que le fichier source de A existe toujours
3. **Test de cache** : Vérifier que les fichiers de cache sont bien supprimés quand non utilisés
4. **Test de collision** : Créer artificiellement une collision de fileId, vérifier la protection

## 📝 Conclusion

Le bug est une **erreur de conception architecturale** : le système traite tous les fichiers de la même manière, sans distinguer :
- Les fichiers appartenant à l'utilisateur (sources)
- Les fichiers temporaires de l'application (cache remote)

La solution nécessite d'ajouter cette distinction au niveau du `LocalFileRepository` et de protéger les opérations de suppression dans `FileManager` et `UploadManager`.

**Impact** : Perte de données utilisateur - À corriger en priorité absolue avant toute mise en production.
