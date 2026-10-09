# Flow authoring and compilation values

`NativeCallDefinition::create` freezes the invocation signature used by `NativeFuncCall`.
The returned immutable owner retains all signature names, parameter and result types,
record identity/ancestry and value operations. Record projections deliberately omit
reflected fields/methods: inspection and reflective discovery use the original metadata
provider. The explicit `CodeLease` covers the invoker and any copied type operations.
`CodeLease::builtin()` is only for process-lifetime code; dynamic callers supply its real
code owner. No module loader or second reflection registry is created here.

A node retains this definition before its pin owners. Rebinding keeps the previous
owner until old pins and default values are destroyed. Node/exec identity and the existing
pin reconstruction order remain unchanged; rebuilding data pins still removes their
old links. The source codec and reflection palette construct the same definition.

`FlowAnalysis::create` owns the domain checks previously embedded in Toolchain: Ability
and Event requirements, transitive suspension through graph function calls, borrowed-step
values crossing suspension and synchronous lifecycle exports. It accepts the real FlowGraph
and immutable catalog views without LLVM, Process or Editor. The compiler uses this same
analysis before lowering and when checking generated async markers; there is no second
Toolchain implementation of these rules.

`reachableExecution` and `findBranchMerge` also belong to this module. They query the current
graph and return owned IDs, never borrowed nodes. Direct-edge traversal is iterative and shared
with the immutable suspension projection. The compiler consumes these queries; its old pointer
walk and branch-merge implementation are removed. Merge selection preserves the existing
true-leg-first breadth-first rule; it is not advertised as a general post-dominance theorem.
At a merge owned by an enclosing control region, nested branch lowering returns its outer token
without inspecting that merge's other predecessors. Only the owning chain gathers them, after
its participating regions have returned. The original unmaterialized-token and SSA-dominance
failures are archived; the regression now requires successful compilation and checks linked
native execution for one, two and three nested levels in both predecessor insertion orders.

The result owns requirement values and an execution-reachability projection keyed by the
input graph's stable IDs. It does not retain Node/Pin pointers, catalog views or reflection
metadata. Graph and catalog destruction is safe after analysis. This is a disposable
compilation result, not an editable graph or a live cache: after editing a graph, analyze
the new input again. Queries return the lowest reachable suspension NodeId deterministically,
including recursive functions, or an invalid ID when no suspension is reachable. The caller
must pair analysis and lowering from the same unchanged graph input.

This lifetime contract does not complete the broader Flow graph migration: the existing
Node/Pin structure and NodeRegistry palette are still active. The final domain catalog,
compiler extension surface and removal of duplicate structural authority remain pending.

Node identity belongs to the graph store. `addNode` returns the issued `NodeId`; constructors
only accept semantic data. `nodes()` borrows `{id, node}` entries, and `nodeId(pointer)` is a
derived reverse lookup that returns an invalid ID for detached or foreign nodes. The owning
store uses stable IDs directly, without a recycled container index or an ID inside `Node`.
It orders entries by ID and does not allocate sparse storage proportional to the ID value.

Extraction returns a `FlowNodeSnapshot` containing the key, unique owner and pin identity sequence. Restoration
uses that explicit key; fresh insertion never reuses an issued identity. Batch insertion
uses `FlowNodeInsertion`: zero requests a new ID, a nonzero ID explicitly restores a snapshot.
Preparation keeps input owners untouched and prepares all store entries and reverse indexes;
commit only moves ownership and swaps the prepared storage with the original `GraphEdit`.
Removed snapshots retain their keys for undo. Reverse indexes never issue identities.

Pins no longer contain IDs. `pinId(pointer)` resolves the graph's non-owning reverse index;
`findPin(id)` resolves its hash index without walking nodes or pin arrays. Node members still
own the actual pin objects until the registered semantic payload migration. Topology remains
the sole authority for pin owner/direction/fan/links. The indexes only locate those objects.

Snapshot pin IDs follow input-then-output order. An empty sequence requests fresh signature
pins; otherwise its size must exactly match the signature, and an invalid entry requests a
new pin at that position. Explicit identities are restored before fresh signature identities
are issued. Detached pins have no graph identity; source reconstruction and undo provide the
saved sequence to insertion instead of mutating detached object fields. Exhaustion remains
absorbing, while explicit restoration is still permitted. Preparation builds complete indexes;
commit swaps them without allocating or invoking user code.

This step does not remove graph membership pointers or polymorphic
semantic node classes. The registered Flow payload/compiler migration is still required.

Function calls and function returns store the definition's graph-local `NodeId`, not its
address. Their constructors borrow a definition only to copy the initial pin schema. Resolve
against the receiving graph immediately before use; missing/wrong-kind definitions or a
different pin signature fail resolution. Pin types still borrow the immutable metadata
environment under its existing lifetime contract. Equal numeric IDs in different graphs do
not identify the same object: callers explicitly supply an ID from the receiving graph.

An extracted definition can be restored at a different address without rebinding its users.
Unresolved intermediate drafts remain possible through the low-level store, but source
capture and call lowering refuse them. Batch edits still reject removing a definition while
retaining its users. New definition/call/return candidates in one batch use explicit snapshot
IDs; admission checks the complete candidate set, including definitions appearing later.
The pointer-based constructors and stored-reference accessors have been removed.

## Registered value compilation

`FlowNodeCatalog` publishes immutable, canonical-name/version definitions. Registration only
validates and stores declarations; it does not construct payloads or run extension callbacks.
Definitions own their type-token names and retain a `CodeLease`. Payloads own semantic data,
clone/destruction callbacks and the same code lease; neither owns node identity or topology.
Pin declarations borrow the immutable reflection environment under its existing lifetime
contract. They may describe a dynamic number of pins.

`FlowNodeType::describePins` admits editable schemas independently of compile eligibility.
`compile` validates the payload, input signature and returned output signature. Its synchronous
callback can combine multiple `FlowValueCompiler::emitScalar` primitives. Values are scoped to
that invocation, cannot escape it, and do not identify graph objects. Toolchain translates the
primitives into MLIR; built-in scalar lowering uses this same translation. Failed compilation
discards the candidate, including any primitives already emitted by a rejected callback.

`createFlowValueNode` currently connects these plain payloads to the existing Flow node store.
Its private node adapter retains the definition and owns its declared pin objects; it does not
erase an old polymorphic node into the semantic payload. Topology uses the canonical type and
declared pin semantics. This is an intermediate migration boundary, not completion of MA08:
registered control/native/Ability nodes, canonical source round-trip and the final plain
NodeId/PinId stores still need migration. The old source codec rejects unsupported registered
nodes rather than silently dropping their payload. No additional runtime, executor or plugin
loader is introduced here.
