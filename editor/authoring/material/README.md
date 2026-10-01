# Material author model

`material_model` is a static CPU library installed as `lux-engine-editor-material-model`.
It links the existing material graph, edit_sessions and the sole edit_history implementation.
It creates no window, runtime, preview, compiler or project storage.

SessionStore exclusively owns MaterialSession. The session owns one MaterialSource, SessionState
and History. History is destroyed before its source; outer CodeLease owners outlive both. The
private allocation-free currentContent queries the real History; there is no cached current,
dirty or busy bit. Loaded bound sources start with the existing checkpoint; untitled ones do not.

An ordered MaterialEditBatch creates one history entry. Value-only batches copy only addressed
values and never clone unrelated nodes. Structural batches build an isolated graph candidate,
process every intention in order, and retain only differences for undo/redo. Replacing or
deleting a node ends that node's earlier field state. This structural candidate has O(graph size)
cost; it is not a zero-copy graph or an exact allocator/RSS budget. Staging and retained-history
limits remain separate and enforced. Commit uses prepared graph transfers and value swaps.

Existing pin direction, conversion, identity and slot-reference rules remain. Reconnecting an
already-connected pair is NO_CHANGE; connecting a new source displaces the input's old link.
Cycles, disconnected nodes and missing output remain valid editable intermediate states.
Slots and referencing nodes may be changed together, validated against the final batch graph.
Node placement is serialized author content; camera, pan, zoom, selection and hover are absent.
Root AssetId is not embedded in node mementos.

By-value batches own supplied node candidates, also on failure. They never transfer half a
candidate into the live source. The old product's reference-taking methods clone that input and
consume its original unique_ptr only after success. All pure editing/history algorithms have
one implementation here; old MaterialEditor admission, asset catalog checks, signals, preview,
compile and persistence adapters remain scheduled for P05/P07/P10/P12.

MaterialReadView::withRead lends const source only within a synchronous callback. Owning capture
and encode enter the same SessionState gate; temporary destruction and exception unwinding occur
before releasing it. No borrowed reference survives edits/reload/close. MaterialSnapshot owns an
independent polymorphic graph and its external code leases; its move assignment also preserves
destructor/code ordering. The source codec still recognizes only its existing built-in schemas.
Built-in node subclasses may specialize clone/destruction while keeping that schema and must
honor the independent clone contract; this does not add a new serialized plugin kind or format.

Code owners are deduplicated by shared ownership identity and conservatively retained for the
source/history lifetime (until reload/close); each snapshot keeps its own owners. A lease inside
a plugin-defined node would expire too soon for the returning destructor and is not used.
PreparedMaterialReload is an installed domain preparation capability; it adopts a complete candidate and new History
atomically while retaining SessionId. It provides no IO or public mark-clean/replace-source path.

The native cases test full encoded graphs and history/checkpoint state on rejection, mixed
node lifetimes, deep snapshots, callback reentry/unwind, and late code/allocator destruction.
The installed consumer uses only the public CPU package. Dependency negatives use real CMake
graphs, including aliases, transitive LINK_ONLY and imported interfaces. Product regressions
remain mandatory; P03 does not claim the later preview/compiler/persistence migration complete.
