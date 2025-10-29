# État de la Migration - Phase 3

## ✅ Complété (Phase 1 + 2 + 3 partiel)

### Architecture Persistente
- **Client ID Stable** : Chaque instance client génère et persiste un UUID unique
  - Stockage : QSettings avec clé composite (machineId + installHash + instanceSuffix)
  - Envoyé dans tous les `register` comme `clientId`
  - Support multi-instance via `MOUFFETTE_INSTANCE_SUFFIX` ou `--instance-suffix`

### Serveur Intelligent
- **Session Management** :
  - `clients` Map : socketId → session info (éphémère)
  - `sessionsByPersistent` Map : persistentId → Set(sessionIds)
  - Fusion automatique lors reconnexion avec même persistentId

- **Upload Tracking** :
  - `uploads` Map : uploadId → {sender, target, canvasSessionId, files, startTime}
  - `clientFiles` Map : persistentClientId → Map(canvasSessionId → Set(fileId))
  - Nettoyage automatique des uploads complétés/aborted

- **State Sync** :
  - Message `state_sync` envoyé lors reconnexion
  - Contient toutes les ideas avec leurs fileIds
  - Client restaure automatiquement les scènes

### Validation Stricte (Phase 3)
- **Rejets Serveur** :
  - ❌ `register` sans `clientId` persistant
  - ❌ `upload_start/complete` sans `canvasSessionId`
  - ❌ `remove_all_files/remove_file` sans `canvasSessionId`
  - ❌ `request_screens` sur cible sans `persistentId`

- **Client Guards** :
  - UploadManager bloque toute action si `canvasSessionId` vide
  - Logs d'avertissement explicites

- **Protocole** :
  - `client_list` filtre uniquement clients avec `persistentId`
  - `screens_info` inclut toujours `persistentClientId`
  - Plus de fallback vers `clientId` legacy

## 🔄 En Cours

### Tests End-to-End
- [ ] Scénario 1 : Upload + Reconnexion simple
- [ ] Scénario 2 : Nouveau sessionId après reconnexion
- [ ] Scénario 3 : Deux instances sur même machine
- [ ] Scénario 4 : Upload interrompu + reconnexion
- [ ] Scénario 5 : Validation canvasSessionId obligatoire
- [ ] Scénario 6 : Client sans persistentId

### Cleanup Restant
- [ ] Supprimer tous les commentaires/code mort mentionnant "identityKey"
- [ ] Audit des chemins de code pour éliminer derniers fallbacks
- [ ] Documentation protocole mise à jour

## 📋 Prochaines Étapes (Phase 4)

### Refactoring Architecture
1. **SessionManager** :
   ```cpp
   class SessionManager {
       - QHash<QString, CanvasSession> sessions
       - QString activeSessionId
       - CanvasSession* ensureSession(ClientInfo)
       - void switchTo(QString persistentId)
       - void cleanup(QString persistentId)
   }
   ```

2. **FileManager Split** :
   - `FileRegistry` : file metadata + deduplication
   - `UploadStateTracker` : per-client upload status
   - `IdeaManager` : idea lifecycle + file associations

3. **Dependency Injection** :
   - Remplacer singletons par injection
   - Interfaces pour testabilité
   - Factory patterns pour création d'objets

4. **Tests Unitaires** :
   - SessionManager isolation tests
   - FileRegistry deduplication tests
   - UploadStateTracker persistence tests
   - Mock WebSocketClient pour tests offline

## 📊 Métriques

### Code Coverage (estimé)
- Phase 1 : 100% ✅
- Phase 2 : 100% ✅
- Phase 3 : ~70% 🔄
- Phase 4 : 0% ⏳

### Stabilité
- Reconnexion simple : ✅ Testé manuellement
- Multi-instance : ✅ Testé manuellement
- Upload persistence : ✅ Testé manuellement
- Edge cases : ⏳ Tests automatisés requis

### Performance
- Temps reconnexion : < 500ms (bon)
- Taille state_sync : ~1KB pour 10 fichiers (acceptable)
- Mémoire serveur : O(clients × ideas × files) (acceptable pour < 100 clients)

## 🐛 Bugs Connus

### Critique
- Aucun identifié ✅

### Mineurs
- Logs serveur pourraient être plus verbeux sur rejets
- Client ne notifie pas visuellement les erreurs serveur (sauf logs)

## 🔐 Sécurité

### Validations Actuelles
- ✅ ClientId format UUID validé
- ✅ IdeaId requis pour toutes opérations fichiers
- ✅ TargetClientId validé existe avant relay

### À Ajouter (Phase 4+)
- [ ] Rate limiting sur uploads
- [ ] Validation taille fichiers
- [ ] Authentification clients (optionnel)
- [ ] Chiffrement payload fichiers (optionnel)

## 📚 Documentation

### À Jour
- ✅ RECONNECTION_TEST_SCENARIOS.md
- ✅ VIDEO_RENDERING_ANALYSIS.md
- ✅ README.md (général)

### À Créer
- [ ] API_PROTOCOL.md (spec complète WebSocket)
- [ ] ARCHITECTURE.md (diagrammes composants)
- [ ] MIGRATION_GUIDE.md (pour déploiement production)
