# Fix: Directional Session IDs

## Problem Summary

The application had a critical architectural flaw where canvas sessions in both directions (A→B and B→A) shared the same `canvasSessionId`. This caused:

1. **Upload State Contamination**: When Client B uploaded files to A, the cleanup process would query `getFileIdsForIdea(canvasSessionId)` which returned files from BOTH directions, incorrectly removing tracking data for files A had sent to B.

2. **Global Overlay Refresh**: `dispatchUploadStateChanged()` would refresh ALL canvas overlays, propagating incorrect state across unrelated sessions.

3. **File Deletion Risk**: Cleanup code could potentially delete source files instead of just cache files due to shared fileId tracking.

## Root Cause

```cpp
// OLD CODE - SessionManager.cpp
newSession.canvasSessionId = DEFAULT_IDEA_ID; // ❌ Same for all sessions
```

Both A→B and B→A used the same session ID, treating them as a shared collaborative space instead of independent unidirectional preparation zones.

## Solution Implemented

### 1. Directional Session IDs in SessionManager

**SessionManager.h**:
```cpp
// Added:
QString m_myClientId; // Local client preparing scenes
QString generateDirectionalSessionId(const QString& source, const QString& target) const;
void setMyClientId(const QString& myClientId);
```

**SessionManager.cpp**:
```cpp
QString SessionManager::generateDirectionalSessionId(const QString& sourceClientId, const QString& targetClientId) const {
    // Format: "sourceClient_TO_targetClient_canvas_uuid"
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QString("%1_TO_%2_canvas_%3").arg(sourceClientId, targetClientId, uuid);
}

// In getOrCreateSession():
if (m_myClientId.isEmpty()) {
    newSession.canvasSessionId = DEFAULT_IDEA_ID;
} else {
    newSession.canvasSessionId = generateDirectionalSessionId(m_myClientId, persistentClientId);
}
```

### 2. Directional Incoming Sessions in UploadManager

**UploadManager.h**:
```cpp
// Added:
QString m_myClientId;
void setMyClientId(const QString& myClientId);
```

**UploadManager.cpp** - `handleIncomingMessage`:
```cpp
// CRITICAL FIX: Generate DIRECTIONAL canvasSessionId
// Format: "senderClient_TO_myClient_canvas_uploadId"
QString directionalSessionId = QString("%1_TO_%2_canvas_%3")
    .arg(m_incoming.senderId, m_myClientId, m_incoming.uploadId);
m_incoming.canvasSessionId = directionalSessionId;
```

### 3. Initialization in WebSocketMessageHandler

**WebSocketMessageHandler.cpp** - `onConnected()`:
```cpp
// Set SessionManager's local client ID
if (m_mainWindow->getSessionManager() && m_mainWindow->getWebSocketClient()) {
    QString myClientId = m_mainWindow->getWebSocketClient()->getPersistentClientId();
    m_mainWindow->getSessionManager()->setMyClientId(myClientId);
}

// Set UploadManager's local client ID
if (m_mainWindow->getUploadManager() && m_mainWindow->getWebSocketClient()) {
    QString myClientId = m_mainWindow->getWebSocketClient()->getPersistentClientId();
    m_mainWindow->getUploadManager()->setMyClientId(myClientId);
}
```

## How It Works

### Session Creation (A prepares scene for B)

**Client A** connects to **Client B**'s canvas:
```
A's persistentClientId: "clientA_abc"
B's persistentClientId: "clientB_def"

SessionManager generates:
canvasSessionId = "clientA_abc_TO_clientB_def_canvas_uuid123"
```

**Client B** connects to **Client A**'s canvas:
```
B's persistentClientId: "clientB_def"
A's persistentClientId: "clientA_abc"

SessionManager generates:
canvasSessionId = "clientB_def_TO_clientA_abc_canvas_uuid456"
```

✅ **Result**: Two completely independent session IDs

### File Tracking Isolation

**A uploads file to B**:
```cpp
m_fileManager->associateFileWithIdea(fileId, "clientA_abc_TO_clientB_def_canvas_uuid123");
```

**B uploads file to A**:
```cpp
m_fileManager->associateFileWithIdea(fileId, "clientB_def_TO_clientA_abc_canvas_uuid456");
```

✅ **Result**: Files tracked separately per direction

### Cleanup Isolation

**B completes upload to A**, cleanup runs:
```cpp
QString canvasSessionId = "clientB_def_TO_clientA_abc_canvas_uuid456";
QSet<QString> ideaFiles = m_fileManager->getFileIdsForIdea(canvasSessionId);
// Returns ONLY files B sent to A, NOT files A sent to B
```

✅ **Result**: No cross-contamination

## Expected Behavior After Fix

1. ✅ **Client A** uploads media to **Client B**
   - A's canvas shows "Uploaded"
   - A's FileManager tracks: `fileA → "clientA_TO_clientB_canvas_xxx"`

2. ✅ **Client B** uploads media to **Client A**
   - B's canvas shows "Uploaded"
   - B's FileManager tracks: `fileB → "clientB_TO_clientA_canvas_yyy"`
   - **A's canvas STILL shows "Uploaded"** ← THIS WAS THE BUG

3. ✅ **Cleanup on A** (when B's upload completes):
   - Queries: `getFileIdsForIdea("clientB_TO_clientA_canvas_yyy")`
   - Returns: Only `fileB`
   - Does NOT affect `fileA` which is in a different session

## Files Modified

- `client/src/backend/domain/session/SessionManager.h`
- `client/src/backend/domain/session/SessionManager.cpp`
- `client/src/backend/network/UploadManager.h`
- `client/src/backend/network/UploadManager.cpp`
- `client/src/backend/handlers/WebSocketMessageHandler.cpp`

## Testing Checklist

- [ ] Client A uploads file to B → status shows "Uploaded"
- [ ] Client B uploads file to A → B shows "Uploaded", **A still shows "Uploaded"**
- [ ] Disconnect and reconnect → upload states preserved correctly
- [ ] Delete media on remote side → only remote cache files deleted, source files protected
- [ ] Multiple back-and-forth uploads → no state corruption

## Migration Notes

Existing sessions with `DEFAULT_IDEA_ID` will continue to work but won't have directional isolation. New sessions created after this fix will automatically use directional IDs.

If needed, add migration logic to convert existing sessions:
```cpp
if (oldSessionId == DEFAULT_IDEA_ID) {
    newSessionId = generateDirectionalSessionId(myClientId, targetClientId);
    // Update FileManager associations
}
```
