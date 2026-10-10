# Flow authoring and compilation values

## Authority and storage

`FlowGraph` owns plain `FlowNode` values keyed by `NodeId` and plain `FlowPinPayload`
values keyed by `PinId`. `GraphTopology` alone owns membership, pin direction, fan limits
and links. `GraphLayout` alone owns placement. Neither semantic payload stores its own
ID, a graph pointer, a parent node pointer or a duplicate link list. Pin payload lookup
uses a hash map; sparse allocation does not grow with the numeric ID value.

`node(id)` is a synchronous read-only borrow. `nodes()` borrows `{id, node}` entries.
`pin(id)` accesses the domain value; it cannot change topology. Interactive `connect`
and `disconnect` normalize either endpoint order, then use the same `GraphEdit` as batches.
Graph-local IDs must come from the supplied graph; equal numbers in different graphs do
not establish cross-graph identity. Pointer-based connection/discovery APIs are removed.

## Transactions and replay

`FlowGraphEdit` prepares domain candidates together with the existing `graph::GraphEdit`.
Preparation clones input values, validates schemas, references, links and placement, and
reserves destination storage. Failure preserves the published source and caller's candidates.
Issued identities are never recycled, including identities consumed by abandoned preparation.
Explicit restored IDs are admitted before fresh IDs, including definitions referenced by
candidates earlier in the batch. Node/Pin maximum IDs remain valid; exhaustion stops fresh
issuance without preventing explicit restoration.

Commit transfers prepared storage without allocation or extension callbacks. Detached
`FlowNodeSnapshot` values retain the removed node, exact pin records/defaults, links and
layout. They are replay inputs, not another live topology. Pins are destroyed before the
node's metadata/code providers, including during graph and snapshot move assignment.

Dynamic schema changes replace a node through the same transaction. `SequencePayload`
declares extra outputs; `NativeCallPayload` retains an immutable invocation definition.
Retained PinIds, surviving links and layout are explicit inputs. There is no incremental
node mutation, implicit link repair or parallel batch implementation.

Function call/return payloads store graph-local definition IDs and semantic signatures.
Batch validation checks the complete candidate graph. Removing a referenced definition or
changing its kind/signature without updating users is rejected. Capture and compilation
also validate references at their own boundary. Restoring a definition at a different
address does not require pointer rebinding.

## Registrations and compilation

`FlowNodeCatalog` publishes immutable canonical-name/version definitions. Registration
validates declarations without constructing payloads or invoking extension callbacks.
Definitions and erased payloads retain actual `CodeLease` owners. Plugin-local catalog
implementations use the stable Object provider's control-block bridge so final release,
callback cleanup and late weak release cannot return into unloaded code.

Builtins and extensions use the same payload creation, schema, validation and compilation
contracts. A definition provides either a value compiler or an execution compiler.
`FlowValueCompiler` expresses scalar and memory operations; `FlowExecutionCompiler`
expresses control flow, calls, suspension and writes using declared pin semantics.
Toolchain owns SSA, region scope and token lowering. Registered callbacks do not mutate
an authoring graph. Memory readers are re-evaluated after writes rather than cached as pure
values. Schema declaration order determines argument/output order, never numeric PinId order.

Editable schemas remain distinct from compile eligibility: an invalid draft can be captured
when its structure and references are valid, while compilation reports its semantic error.
An extension failure invalidates the disposable compiler candidate; it is never published.

## Metadata and native calls

`NativeCallDefinition::create` freezes signature names, parameter/result types, record
identity/ancestry and value operations. Record projections deliberately omit reflected
fields/methods: discovery uses the original metadata provider. Its explicit `CodeLease`
covers invocation and copied operations. `CodeLease::builtin()` is only for process-lifetime
code. Native schema storage remains execution, parameters, optional Self; invocation places
Self first. Source reconstruction and the reflection palette use this same definition.

General reflection metadata remains borrowed from an immutable environment that outlives
its graphs and snapshots. Script Ability/Event payload clones share their immutable owned
metadata allocation, so restored pin values retain the exact metadata backing they reference.
No second reflection registry or module loader is introduced.

## Source and analysis

Source v2 stores canonical names, versions, semantic pin identities and owning values.
The frozen v1 reader remains supported. Historical builtin wire tags are private codec
implementation details, not runtime operation IDs or extensible compiler dispatch.
Each immutable definition owns one `capture_source` / `restore_source` pair. Builtin
providers resolve their own types, reflected fields, function signatures and Script contracts;
custom providers use the same public contract and preserve their owning `FlowSourcePayload`
bytes. Source values are declared independently in `FlowSourceData.hpp`; the graph loader
does not inspect concrete payload types or dispatch on historical builtin wire tags.

Restoration callbacks synchronously borrow an immutable `FlowSourceEnvironment` and a
`FlowReferenceView` over the unpublished candidate. They must not retain either view.
Definitions declare `DECLARATION` or `BODY` restoration stage, preserving forward function
references without publishing a partial graph. Node lookups expose declarations only;
earlier body nodes do not leak traversal order into extension behavior. This is a two-stage source contract, not a
runtime dependency scheduler. Definition identity/version, code ownership and resulting
pin schemas are checked before admission. Original v1/v2 parameter schemas remain unchanged.

Materialization resolves metadata and forward references, builds detached values and admits
the entire graph through one `FlowGraphEdit`. Source capture validates reference kind and
signature, not just existence. Unknown kinds, versions and invalid references fail explicitly.

`FlowAnalysis` owns Ability/Event requirements, transitive suspension, borrowed-step crossing
and synchronous lifecycle-export checks without LLVM, Process or Editor. Its result owns
values and stable IDs, not source pointers. Toolchain uses the same analysis. Re-analyze after
editing; the result is not a live cache. `reachableExecution` and `findBranchMerge` use the
same topology and schema order. The original true-leg-first breadth-first merge selection is
preserved; it is not a general post-dominance algorithm.

This plain-store closure does not certify the whole MA08 graph-editor rendering gate or
later mechanism stages. Qualification records distinguish development, clean tracked source,
installed SDK and real rendering evidence.
