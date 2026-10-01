# Scene author model

`scene_model` is a static CPU-only library. It creates neither a SceneInstance nor
a SceneRuntime. The installed package is `lux-engine-editor-scene-model`.

## Ownership and edits

SessionStore exclusively owns SceneSession. The session owns SessionState, one
SceneSource and the existing EditHistory; history is destroyed before its source.
SessionState alone owns admission, binding, checkpoint and observation version.
The private allocation-free `currentContent()` reads the actual history state.

SceneSource's private Registry contains recognized author components. Its UUID
map uses the existing WorldObjectId. Object rows keep partition membership and
unrecognized payload only, not another copy of recognized values. Formal World,
Simulation and Scene assets are cloned through the existing codecs on entry.
Editor camera, selection, runtime state and viewport output are not source data.

`read()` is a synchronous borrow invalidated by any edit, reload or close. It does
not expose Registry or component pointers. Component reads return owning encoded
values. Persistent addresses contain SessionId, HistoryId and WorldObjectId;
they do not use a file-root AssetId or runtime Entity as author identity.

`apply()` checks an exact ContentStamp and uses the existing edit gate. All edits
in a batch produce one history operation. Structure changes prepare an isolated
complete CPU candidate before swapping it into place; the history retains only
the object delta, not a second live world. Field-only batches decode and swap the
affected components; configuration-only batches clone and swap configuration.
Commit performs prepared swaps; changes publish after the history commit.
Undo/redo use the same EditHistory algorithm. No second undo stack exists.

Deletion lists are explicit. Unresolved known references fail candidate creation;
unknown payload blocks deletion because its references cannot be safely rewritten.
Reparenting preserves local transforms. Content edits in indexed worlds and
partition/index/storage changes are refused until an appropriate rebuild exists.
This restriction also applies to fields that could invalidate a spatial index.

Model insertion takes an already loaded ModelAsset and returns author object
values; camera creation takes camera/transform values. Neither performs IO.
Legacy SceneEditor adapters reuse these algorithms and must be removed by P12.

## Capture, changes and reload

Capture synchronously encodes every recognized component into independent bytes,
copies unknown payload and opaque package/storage bytes, and clones the three
formal assets. It never freezes only the outer shared_ptr of a plugin value.
Schema code owners outlive Registry values, mementos and snapshots. Snapshot
copies contain immutable bytes, not pointers into a live session.

SnapshotBudget bounds accounted payload and container/configuration storage;
codec decode limits also constrain transient decoded data. It is not an exact
allocator/RSS quota or an OOM recovery policy. Shared plugin module code and schema
catalog backing storage are retained by reference, not duplicated or charged as
fresh module images. Structural staging is proportional to scene content; local
field preparation does not decode unrelated objects. No zero-copy claim is made.

The change journal has explicit record and byte limits. Pruned, future or foreign
cursors return RESET_REQUIRED, including whole-history replacement. Observed
version is not dirty state. Untitled sessions have no checkpoint; a bound loaded
source begins at a clean checkpoint. Undo can return to that exact baseline.

PreparedSceneReload is an installed domain preparation capability; activities own IO and final review. It prepares a matching source
and fresh history before adoption; a stale candidate leaves the session untouched.
It implements no file IO, Save As, dialogs or public mark-clean/replace-source API.

## Validation

Native cases cover real CPU edits, atomic rejection, UUID identity, bounded
changes/reload, plugin-node deep capture and deleter/code lifetime. The installed
consumer links only the public package. P02 checks both actual target closure and
real CMake forbidden-edge fixtures; it does not infer safety from target names.
Existing product, history and session regressions remain required.
