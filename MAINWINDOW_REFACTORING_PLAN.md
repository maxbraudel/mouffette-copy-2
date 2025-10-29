# Plan de Refactoring : Décomposition de MainWindow.cpp

## 📊 Analyse du fichier actuel

**Statistiques :**
- **Lignes de code :** 4,164 lignes
- **Nombre de méthodes :** ~70 méthodes
- **Responsabilités multiples :** Au moins 8 domaines différents

## 🎯 Problèmes identifiés

### 1. Violation du principe de responsabilité unique (SRP)
`MainWindow` gère actuellement :
- Interface UI (création des pages, layouts)
- Gestion de connexion réseau
- Gestion des sessions canvas
- Gestion du volume système
- Synchronisation des états
- Gestion des uploads
- Gestion des animations
- Styles et thèmes
- System tray
- Menus
- Configuration (settings)

### 2. Complexité cognitive élevée
- Trop de dépendances directes
- Logique métier mélangée avec logique UI
- Difficile à tester unitairement
- Difficile à maintenir

### 3. Couplage fort
- De nombreux composants sont créés et gérés directement dans MainWindow
- Les interactions entre composants passent par MainWindow comme médiateur

---

## 📦 Plan de décomposition proposé

### **Phase 1 : Extraction des pages UI (PRIORITÉ HAUTE)**

#### 1.1 `ClientListPage.h/cpp`
**Responsabilité :** Gestion complète de la page de liste des clients

**Contenu extrait de MainWindow :**
```cpp
// Méthodes
- createClientListPage()
- ensureClientListPlaceholder()
- ensureOngoingScenesPlaceholder()
- applyListWidgetStyle()
- refreshOngoingScenesList()
- onClientItemClicked()
- onOngoingSceneItemClicked()
- updateClientList()

// Membres
- m_clientListPage
- m_clientListLabel
- m_clientListWidget
- m_ongoingScenesLabel
- m_ongoingScenesList
```

**Interface publique :**
```cpp
class ClientListPage : public QWidget {
    Q_OBJECT
public:
    explicit ClientListPage(SessionManager* sessionManager, QWidget* parent = nullptr);
    void updateClientList(const QList<ClientInfo>& clients);
    void refreshOngoingScenesList();
    
signals:
    void clientSelected(const ClientInfo& client);
    void ongoingSceneSelected(const QString& persistentClientId);
    
private:
    void ensurePlaceholders();
    void applyListWidgetStyle(QListWidget* listWidget);
    
    SessionManager* m_sessionManager;
    QListWidget* m_clientListWidget;
    QListWidget* m_ongoingScenesList;
    // ... autres membres
};
```

**Bénéfices :**
- ✅ Réduction de ~200 lignes de MainWindow
- ✅ Responsabilité unique et claire
- ✅ Testable indépendamment
- ✅ Réutilisable

---

#### 1.2 `CanvasViewPage.h/cpp`
**Responsabilité :** Gestion complète de la page de visualisation canvas

**Contenu extrait de MainWindow :**
```cpp
// Méthodes
- createScreenViewPage()
- showScreenView()
- updateClientNameDisplay()
- initializeRemoteClientInfoInTopBar()
- createRemoteClientInfoContainer()
- setRemoteConnectionStatus()
- removeRemoteStatusFromLayout()
- addRemoteStatusToLayout()
- removeVolumeIndicatorFromLayout()
- addVolumeIndicatorFromLayout()
- updateVolumeIndicator()
- refreshOverlayActionsState()

// Membres
- m_screenViewWidget
- m_screenViewLayout
- m_clientNameLabel
- m_remoteConnectionStatusLabel
- m_remoteClientInfoContainer
- m_remoteClientInfoWrapper
- m_remoteInfoSep1/2
- m_volumeIndicator
- m_canvasContainer
- m_canvasStack
- m_canvasHostStack
- m_screenCanvas
- m_loadingSpinner
- m_uploadButton
- m_backButton
```

**Interface publique :**
```cpp
class CanvasViewPage : public QWidget {
    Q_OBJECT
public:
    explicit CanvasViewPage(
        SessionManager* sessionManager,
        UploadManager* uploadManager,
        QWidget* parent = nullptr
    );
    
    void showCanvas(ScreenCanvas* canvas);
    void setClientInfo(const ClientInfo& client);
    void setRemoteConnectionStatus(const QString& status);
    void updateVolumeIndicator(int volumePercent);
    ScreenCanvas* currentCanvas() const;
    
signals:
    void backButtonClicked();
    void uploadButtonClicked();
    void remoteConnectionLost();
    
private:
    void createRemoteInfoContainer();
    void updateOverlayActionsState(bool enabled);
    
    SessionManager* m_sessionManager;
    UploadManager* m_uploadManager;
    ScreenCanvas* m_currentCanvas;
    // ... autres membres
};
```

**Bénéfices :**
- ✅ Réduction de ~400 lignes de MainWindow
- ✅ Séparation claire entre les deux pages principales
- ✅ Gestion d'état locale à la page

---

### **Phase 2 : Extraction de la logique de connexion (PRIORITÉ HAUTE)**

#### 2.1 `ConnectionManager.h/cpp`
**Responsabilité :** Gestion complète du cycle de vie de la connexion WebSocket

**Contenu extrait de MainWindow :**
```cpp
// Méthodes
- connectToServer()
- onConnected()
- onDisconnected()
- onConnectionError()
- scheduleReconnect()
- attemptReconnect()
- syncRegistration()
- onRegistrationConfirmed()

// Membres
- m_webSocketClient
- m_reconnectTimer
- m_reconnectAttempts
- m_maxReconnectDelay
- m_serverUrlConfig
```

**Interface publique :**
```cpp
class ConnectionManager : public QObject {
    Q_OBJECT
public:
    explicit ConnectionManager(WebSocketClient* wsClient, QObject* parent = nullptr);
    
    void connectToServer(const QString& serverUrl);
    void disconnect();
    bool isConnected() const;
    QString getServerUrl() const;
    void setServerUrl(const QString& url);
    
signals:
    void connected();
    void disconnected();
    void connectionError(const QString& error);
    void statusChanged(const QString& status);
    void registrationConfirmed(const ClientInfo& clientInfo);
    
private slots:
    void onConnected();
    void onDisconnected();
    void scheduleReconnect();
    void attemptReconnect();
    
private:
    WebSocketClient* m_wsClient;
    QTimer* m_reconnectTimer;
    int m_reconnectAttempts;
    QString m_serverUrl;
};
```

**Bénéfices :**
- ✅ Logique de reconnexion isolée
- ✅ Testable avec mocks
- ✅ Réutilisable dans d'autres contextes
- ✅ Réduction de ~200 lignes

---

### **Phase 3 : Extraction de la logique système (PRIORITÉ MOYENNE)**

#### 3.1 `SystemMonitor.h/cpp`
**Responsabilité :** Surveillance des informations système (volume, écrans, etc.)

**Contenu extrait de MainWindow :**
```cpp
// Méthodes
- getSystemVolumePercent()
- setupVolumeMonitoring()
- getLocalScreenInfo()
- getMachineName()
- getPlatformName()
- onDataRequestReceived()
- handleApplicationStateChanged()

// Membres
- m_cachedSystemVolume
- m_volProc (macOS)
- m_volTimer
```

**Interface publique :**
```cpp
class SystemMonitor : public QObject {
    Q_OBJECT
public:
    explicit SystemMonitor(QObject* parent = nullptr);
    
    int getSystemVolumePercent() const;
    QList<ScreenInfo> getLocalScreenInfo() const;
    QString getMachineName() const;
    QString getPlatformName() const;
    
    void startMonitoring();
    void stopMonitoring();
    
signals:
    void volumeChanged(int volumePercent);
    void screenConfigurationChanged(const QList<ScreenInfo>& screens);
    void applicationStateChanged(Qt::ApplicationState state);
    
private:
    void setupVolumeMonitoring();
    int m_cachedVolume;
    QTimer* m_volumeTimer;
#ifdef Q_OS_MACOS
    QProcess* m_volumeProcess;
#endif
};
```

**Bénéfices :**
- ✅ Isolation du code platform-specific
- ✅ Testable avec mocks pour chaque plateforme
- ✅ Réduction de ~150 lignes

---

#### 3.2 `ThemeManager.h/cpp`
**Responsabilité :** Gestion des thèmes et styles globaux

**Contenu extrait de MainWindow :**
```cpp
// Méthodes
- updateStylesheetsForTheme()
- event() // uniquement la partie PaletteChange

// Variables globales du namespace anonyme (lignes 141-177)
- gWindowContentMarginTop/Right/Bottom/Left
- gWindowBorderRadiusPx
- gInnerContentGap
- gDynamicBoxMinWidth/Height/BorderRadius/FontPx
- gRemoteClientContainerPadding
- gTitleTextFontSize/Height

// Fonctions helpers
- applyPillBtn()
- applyPrimaryBtn()
- applyStatusBox()
- applyTitleText()
- uploadButtonMaxWidth()
```

**Interface publique :**
```cpp
class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager* instance();
    
    // Style configuration
    struct StyleConfig {
        int windowBorderRadius;
        int innerContentGap;
        int dynamicBoxHeight;
        int dynamicBoxBorderRadius;
        // ... autres configs
    };
    
    StyleConfig getStyleConfig() const;
    void setStyleConfig(const StyleConfig& config);
    
    // Helper methods pour appliquer les styles
    void applyPillButton(QPushButton* button);
    void applyPrimaryButton(QPushButton* button);
    void applyStatusBox(QLabel* label, const QString& status);
    void applyTitleText(QLabel* label);
    
signals:
    void themeChanged();
    void styleConfigChanged();
    
private:
    ThemeManager(QObject* parent = nullptr);
    static ThemeManager* s_instance;
    StyleConfig m_config;
};
```

**Bénéfices :**
- ✅ Centralisation de toute la logique de style
- ✅ Configuration facile des constantes de style
- ✅ Réduction de ~200 lignes
- ✅ Pattern Singleton pour accès global

---

### **Phase 4 : Extraction de widgets custom (PRIORITÉ MOYENNE)**

#### 4.1 `RoundedContainer.h/cpp`
**Responsabilité :** Widget avec bord arrondi réutilisable

```cpp
class RoundedContainer : public QWidget {
    Q_OBJECT
public:
    explicit RoundedContainer(QWidget* parent = nullptr);
    void setBorderRadius(int radius);
    
protected:
    void paintEvent(QPaintEvent* event) override;
    
private:
    int m_borderRadius;
};
```

#### 4.2 `ClippedContainer.h/cpp`
**Responsabilité :** Widget avec clipping pour contenu arrondi

```cpp
class ClippedContainer : public QWidget {
    Q_OBJECT
public:
    explicit ClippedContainer(QWidget* parent = nullptr);
    void setBorderRadius(int radius);
    
protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    
private:
    void updateMaskIfNeeded();
    QSize m_lastMaskSize;
    int m_borderRadius;
};
```

#### 4.3 `ClientListDelegate.h/cpp`
**Responsabilité :** Delegate pour affichage des séparateurs dans la liste de clients

```cpp
class ClientListSeparatorDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, 
               const QModelIndex& index) const override;
};
```

**Bénéfices :**
- ✅ Widgets réutilisables dans d'autres parties de l'app
- ✅ Code plus propre dans MainWindow
- ✅ Réduction de ~150 lignes

---

### **Phase 5 : Extraction de la logique métier (PRIORITÉ MOYENNE)**

#### 5.1 `CanvasSessionController.h/cpp`
**Responsabilité :** Coordination entre SessionManager et UI

**Contenu extrait de MainWindow :**
```cpp
// Méthodes
- ensureCanvasSession()
- findCanvasSession() (toutes les variantes)
- configureCanvasSession()
- switchToCanvasSession()
- updateUploadButtonForSession()
- unloadUploadsForSession()
- createIdeaId()
- rotateSessionIdea()
- reconcileRemoteFilesForSession()
- handleStateSyncFromServer()
- markAllSessionsOffline()
- canvasForClientId()
- sessionForActiveUpload()
- sessionForUploadId()
- clearUploadTracking()
- hasUnuploadedFilesForTarget()
```

**Interface publique :**
```cpp
class CanvasSessionController : public QObject {
    Q_OBJECT
public:
    explicit CanvasSessionController(
        SessionManager* sessionManager,
        UploadManager* uploadManager,
        FileManager* fileManager,
        QObject* parent = nullptr
    );
    
    CanvasSession* ensureSession(const ClientInfo& client);
    CanvasSession* findSession(const QString& persistentClientId);
    CanvasSession* findSessionByServerId(const QString& serverId);
    
    void switchToSession(const QString& persistentClientId);
    void reconcileRemoteFiles(const QString& persistentClientId, 
                              const QSet<QString>& fileIds);
    void handleStateSync(const QJsonObject& message);
    void markAllSessionsOffline();
    
    bool hasUnuploadedFiles(const QString& targetClientId) const;
    
signals:
    void sessionCreated(CanvasSession* session);
    void sessionSwitched(CanvasSession* session);
    void remoteFilesReconciled(const QString& persistentClientId);
    
private:
    SessionManager* m_sessionManager;
    UploadManager* m_uploadManager;
    FileManager* m_fileManager;
};
```

**Bénéfices :**
- ✅ Séparation logique métier / UI
- ✅ Testable avec mocks
- ✅ Réduction de ~400 lignes

---

### **Phase 6 : Extraction de composants UI spécialisés (PRIORITÉ BASSE)**

#### 6.1 `ConnectionStatusBar.h/cpp`
**Responsabilité :** Barre de statut de connexion en haut

**Contenu :**
```cpp
- createLocalClientInfoContainer()
- setLocalNetworkStatus()
- updateConnectionStatus()

// Membres
- m_connectionBar
- m_connectionLayout
- m_settingsButton
- m_connectToggleButton
- m_connectionStatusLabel
- m_localClientInfoContainer
- m_localClientTitleLabel
- m_localNetworkStatusLabel
```

#### 6.2 `SettingsDialog.h/cpp`
**Responsabilité :** Dialog de configuration

**Contenu :**
```cpp
- showSettingsDialog()
- onEnableDisableClicked()
```

#### 6.3 `SystemTrayManager.h/cpp`
**Responsabilité :** Gestion du system tray

**Contenu :**
```cpp
- setupSystemTray()
- onTrayIconActivated()

// Membres
- m_trayIcon
```

#### 6.4 `MenuBarManager.h/cpp`
**Responsabilité :** Gestion de la barre de menu (non-macOS)

**Contenu :**
```cpp
- setupMenuBar()

// Membres
- m_fileMenu
- m_helpMenu
- m_exitAction
- m_aboutAction
```

**Bénéfices :**
- ✅ Réduction de ~300 lignes
- ✅ Composants isolés et testables

---

## 📋 Structure finale proposée

```
client/src/
├── MainWindow.h                    (~150 lignes, -190)
├── MainWindow.cpp                  (~1000 lignes, -3164)
│
├── ui/
│   ├── pages/
│   │   ├── ClientListPage.h       (nouveau)
│   │   ├── ClientListPage.cpp     (nouveau, ~200 lignes)
│   │   ├── CanvasViewPage.h       (nouveau)
│   │   └── CanvasViewPage.cpp     (nouveau, ~400 lignes)
│   │
│   ├── widgets/
│   │   ├── RoundedContainer.h     (nouveau)
│   │   ├── RoundedContainer.cpp   (nouveau, ~50 lignes)
│   │   ├── ClippedContainer.h     (nouveau)
│   │   ├── ClippedContainer.cpp   (nouveau, ~60 lignes)
│   │   ├── ClientListDelegate.h   (nouveau)
│   │   └── ClientListDelegate.cpp (nouveau, ~40 lignes)
│   │
│   └── components/
│       ├── ConnectionStatusBar.h  (nouveau)
│       ├── ConnectionStatusBar.cpp (nouveau, ~150 lignes)
│       ├── SettingsDialog.h       (nouveau)
│       ├── SettingsDialog.cpp     (nouveau, ~100 lignes)
│       ├── SystemTrayManager.h    (nouveau)
│       ├── SystemTrayManager.cpp  (nouveau, ~80 lignes)
│       ├── MenuBarManager.h       (nouveau)
│       └── MenuBarManager.cpp     (nouveau, ~80 lignes)
│
├── managers/
│   ├── ConnectionManager.h        (nouveau)
│   ├── ConnectionManager.cpp      (nouveau, ~200 lignes)
│   ├── SystemMonitor.h            (nouveau)
│   ├── SystemMonitor.cpp          (nouveau, ~150 lignes)
│   ├── ThemeManager.h             (nouveau)
│   └── ThemeManager.cpp           (nouveau, ~200 lignes)
│
└── controllers/
    ├── CanvasSessionController.h  (nouveau)
    └── CanvasSessionController.cpp (nouveau, ~400 lignes)
```

---

## 🚀 Plan d'implémentation

### **Ordre d'exécution recommandé :**

1. **Phase 4** (Widgets custom) - COMMENCER ICI
   - Pas de dépendances
   - Extraction mécanique simple
   - Tests rapides
   - **Durée estimée :** 1-2 heures

2. **Phase 2** (ThemeManager seulement)
   - Créer le singleton ThemeManager
   - Migrer les variables globales et helpers
   - **Durée estimée :** 2-3 heures

3. **Phase 1** (Pages UI)
   - ClientListPage d'abord (plus simple)
   - Puis CanvasViewPage
   - **Durée estimée :** 4-6 heures

4. **Phase 3** (ConnectionManager + SystemMonitor)
   - ConnectionManager d'abord
   - Puis SystemMonitor
   - **Durée estimée :** 3-4 heures

5. **Phase 5** (CanvasSessionController)
   - Extraction de la logique métier
   - **Durée estimée :** 4-5 heures

6. **Phase 6** (Composants UI spécialisés)
   - Extraction finale des petits composants
   - **Durée estimée :** 3-4 heures

**Durée totale estimée :** 17-24 heures de développement

---

## ✅ Bénéfices attendus

### Avant refactoring :
- ❌ 4,164 lignes dans MainWindow.cpp
- ❌ ~70 méthodes dans une seule classe
- ❌ 8+ responsabilités différentes
- ❌ Tests difficiles
- ❌ Maintenance complexe

### Après refactoring :
- ✅ ~1,000 lignes dans MainWindow.cpp (-76%)
- ✅ ~20 méthodes dans MainWindow
- ✅ Responsabilité unique : orchestration
- ✅ 14 nouveaux fichiers spécialisés
- ✅ Chaque composant testable indépendamment
- ✅ Code réutilisable
- ✅ Maintenance facile
- ✅ Onboarding simplifié pour nouveaux développeurs

---

## 🎯 Principes respectés

1. **Single Responsibility Principle (SRP)**
   - Chaque classe a une seule raison de changer

2. **Open/Closed Principle (OCP)**
   - Composants extensibles sans modification

3. **Dependency Inversion Principle (DIP)**
   - MainWindow dépend des abstractions (interfaces)
   - Injection de dépendances

4. **Don't Repeat Yourself (DRY)**
   - Styles centralisés dans ThemeManager
   - Widgets réutilisables

5. **Separation of Concerns (SoC)**
   - UI séparée de la logique métier
   - Logique système isolée

---

## 🔧 Stratégie de migration

### Pour chaque extraction :

1. **Créer** le nouveau fichier `.h/.cpp`
2. **Copier** les méthodes et membres concernés
3. **Adapter** les interfaces (constructeurs, signals, slots)
4. **Ajouter** les dépendances nécessaires
5. **Modifier** MainWindow pour utiliser le nouveau composant
6. **Compiler** et corriger les erreurs
7. **Tester** le comportement
8. **Commit** avec message descriptif

### Règle d'or :
> **Chaque extraction doit compiler et fonctionner avant de passer à la suivante**

---

## 📝 Notes importantes

### Dépendances à surveiller :

1. **SessionManager** - utilisé partout
2. **WebSocketClient** - partagé entre plusieurs managers
3. **UploadManager** - interaction avec canvas et sessions
4. **FileManager** - utilisé pour tracking des fichiers
5. **AppColors** - utilisé pour tous les styles

### Injection de dépendances :

MainWindow deviendra principalement un **compositeur** qui :
- Crée les instances des managers
- Injecte les dépendances
- Connecte les signals/slots
- Gère le cycle de vie

### Points d'attention :

- ⚠️ **Ownership Qt** : bien gérer les `parent` pour éviter leaks
- ⚠️ **Signals/Slots** : vérifier toutes les connexions
- ⚠️ **État partagé** : minimiser les variables globales
- ⚠️ **Platform-specific** : bien isoler le code `#ifdef`

---

## 🎉 Conclusion

Ce refactoring transformera MainWindow d'un **monolithe de 4,164 lignes** en un **orchestrateur léger de ~1,000 lignes**, entouré de **14 composants spécialisés, testables et réutilisables**.

Le code sera :
- ✅ Plus maintenable
- ✅ Plus testable
- ✅ Plus lisible
- ✅ Plus évolutif
- ✅ Mieux organisé

**Recommandation : Commencer par la Phase 4 (widgets custom) qui est la plus simple et donnera confiance pour attaquer les phases suivantes.**
