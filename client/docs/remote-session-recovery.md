# Remote session recovery on the Canvas

`ApplicationRuntime` reconciles session intent with authenticated discovery and
`RemoteSessionCoordinator` bindings. A saved Project alone does not request a
session. There are two sources of intent:

- Explicit client selection, including the first OPEN before a Project exists.
- Continued activity on the selected Canvas with a retained Project. The control
  window must be visible, its pointer must be inside, and the system must not be
  suspended. Local disable and shutdown prevent recovery.

Foreground intent is computed from current state, not stored as another pending
selection. Discovery, activity resumption, and authoritative terminal completion
all reconcile it. Consequently, either ordering of discovery and Closed recovers
the session immediately once both permit it. Leaving the Canvas or becoming
inactive removes automatic retry intent, including while cleanup is pending.

The existing session state remains authoritative:

- Active and Grace bindings are reused/resumed, never replaced by another OPEN.
- An in-flight OPEN is deduplicated. After a short transport interruption with
  no known binding, its exact request ID is replayed on the authenticated socket.
  A known binding follows the signed RESUME protocol. Accepted Ready/Resumed
  retires pending OPEN correlations even when the reply has no request ID.
- A pending CLOSE or cancelled initial OPEN fences the old session until its
  exact terminal result. Stale results cannot remove a replacement session.
- Cleanup/resume/runtime conflicts wait for another authenticated discovery
  update. The server publishes these updates at cleanup and registration
  boundaries; errors do not synchronously trigger another OPEN.
- Invalid snapshots and other permanent OPEN rejections block automatic recovery
  until explicit selection. This prevents validation failures from creating an
  OPEN/CLOSE loop. Offline and opening-timeout failures allow later recovery.

Automatic OPEN errors never become persistent explicit intent. A retry after
cleanup therefore rechecks current pointer, visibility, and navigation state.
Recovery keeps the existing Project and Canvas; only an authenticated successful
session restores remote capabilities and installs its fresh snapshot.

`tst_ClientConnectionFlow` exercises both network orderings, close fences,
duplicate discovery and stale closes, activity/navigation changes, transport
replacement during OPEN, transient rejections, and invalid snapshots. Run:

```sh
ctest --test-dir out/build/macos-debug -R ClientConnectionFlow --output-on-failure
```
