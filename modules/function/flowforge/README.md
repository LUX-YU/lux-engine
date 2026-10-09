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

Extraction returns a `FlowNodeSnapshot` containing the key and unique owner. Restoration
uses that explicit key; fresh insertion never reuses an issued identity. Batch insertion
uses `FlowNodeInsertion`: zero requests a new ID, a nonzero ID explicitly restores a snapshot.
Preparation keeps input owners untouched and prepares all store entries and reverse indexes;
commit only moves ownership and swaps the prepared storage with the original `GraphEdit`.
Removed snapshots retain their keys for undo. Reverse indexes never issue identities.

This step does not remove the remaining Pin IDs, graph membership pointers or polymorphic
semantic node classes. The registered Flow payload/compiler migration is still required.
