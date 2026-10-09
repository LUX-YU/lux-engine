# Flow plain stores and registered compilation

Implementation: `6e7a67592ab57fd102dbdfa4832a3ba508e69ff0`, followed by the installed
compiler-consumer correction `1f1e59b8d83fc1455116133d5ba44796e526fa01`.
Final qualification binds the latter SHA. lux-cxx: `0a0e7419fc7229df6e372cd35a540249f92250ef`.

The Flow migration is implemented. **MA06/MA08 and the overall task remain incomplete.**
The final Editor matrix also has an unresolved intermittent transfer-retirement test failure;
this record does not claim a fully passing Editor qualification.

## Actual authority and lifetime

`FlowGraph` owns plain node and pin values keyed by NodeId/PinId. `GraphTopology` is the
only membership, direction, fan and link authority; `GraphLayout` owns placement.
Payloads have no ID, graph back-reference, parent pointer or second link list.
Pin lookup is hash-based and does not allocate storage proportional to sparse numeric IDs.

The existing FlowGraphEdit prepares payloads alongside the one generic GraphEdit. Whole-batch
reference validation sees forward definitions. Explicit IDs precede fresh issuance; rejected or
abandoned preparation never recycles issued identities. Commit transfers prepared storage without
allocation or extension callbacks. Detached snapshots preserve exact pins, defaults, links and
layout. Pin values are cleaned before their metadata/code providers, including move assignment.
Ability/Event payload clones share the same immutable metadata allocation that defaults reference.

Control, function, native, Ability/Event, scalar, variable and field compilation uses registered
value/execution callbacks. Schema order determines semantic arguments and branch order, independent
of PinId numeric order. Reads of mutable state are not cached across writes. The original compiler
and source validation errors remain domain errors; no general manager or parallel history was added.

Removed NodeBase and the old ControlNode, FunctionalNode, ObjectNode, ScriptAbilityNode and
ScriptEventAwaitNode headers/implementations, ENodeOperation and pointer-based graph discovery.
There are no forwarding aliases. The narrow private builtin source tags preserve historical wire
ordinals; they are not runtime compiler dispatch. Metadata-dependent builtin parameter adapters
still remain in the source codec, so complete codec distribution is not claimed.

## Reproduced corrections and preserved behavior

- Reverse-endpoint connection rejection was reproduced before normalization. Interactive calls now
  normalize before the same topology transaction. All 588 original admission decisions match;
  this does not equate retired numeric error enums with the new typed failures.
- Reverse numeric PinIds exposed incorrect branch ordering. Traversal now follows registered schema
  order. Restored source, diamond/tie/cycle and deep-chain assertions remain.
- Source capture incorrectly accepted an existing callee of the wrong kind. Capture now calls the
  definition's reference validator, retaining kind/signature diagnostics and whole-source preservation.
- Restored script defaults exposed metadata lifetime loss. Payload clones now retain their original
  metadata allocation; actual extract/restore and provider destruction are tested.

The original 33 v1 source fixtures, atomic rejection, stable IDs/high water, dynamic schemas,
snapshot replay, default values, native ownership and real extension DLL assertions are retained.
Pointer provenance tests became actual compile rejection of the removed pointer APIs; graph-local
equal numeric IDs are not falsely presented as globally distinct identities.

The first installed graph/compiler build at `6e7a675` found an unmigrated test body that the root
build does not enable. The follow-up changes only that consumer, preserving Material SPIR-V,
Flow object bytes/triple and candidate ownership assertions. The failed output is retained.

## Qualification scope

Independent clean tracked source, reused incremental Editor/PLAYER trees, and a fresh SDK prefix
are used. This is not a cold-build claim. Builds use `all -j 4 -- -k 0`, then verify no new work.
Installed consumers compile against installed public headers and libraries, without private source
include paths or build-DLL fallback.

| Scope | Actual result |
|---|---|
| Development Editor | 170/170; not relabeled as final clean-source evidence |
| Final Editor at `1f1e59b` | 169/170; `render.transfer_idle` failed |
| Targeted transfer-idle replay | All three modes pass; first failure remains unresolved |
| Final PLAYER | 96/96; original 91 names retained, five plain-value tests added |
| SDK analysis / native / scalar | 4/4, 7/7, 7/7 |
| SDK graph / payload / Ability | 4/4, 2/2, 3/3 |

The transfer test's simulated device-loss branch returned zero but lacked its final PASS marker.
The unchanged binary passed targeted replay. No production workaround, weakened assertion or
automatic retry policy was introduced. Its cause is not established and the failed matrix is retained.

Six actual SDK compile negatives reject pointer connect/disconnect (C2664), node/graph back-references
(C2039), ENodeOperation (C2653) and NodeBase.hpp (C1083). Each uses the same positive fixture and
returns to a successful build/run after the illegal expression is removed. The initial fixture
configuration failed on a Windows path escape; that output is retained, not counted as a negative.

Twelve original diagnostics and all three AOT artifacts match byte-for-byte: 123 scalar exports /
28,393 bytes, six control objects / 5,772 payload bytes (5,820 framed), and three async objects /
4,931 framed bytes. Actual execution covers 48 nested-control calls, three registered-provider
paths with 77 calls each, and the read/write regression (1 then 7; next invocation still 7).
AOT generation is distinct from native execution.

CMake File API and actual compile/link commands show the Flow dependency closure is limited to
flowforge, graph, meta, object, script_core and description. Changed installed public headers and
the three development prefixes match; the six obsolete headers are absent. The applicable CTest
GPU cases do not establish graph-editor rendering or deferred interactive-input qualification.

## Evidence and remaining work

External archive:
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/plain-flow-store`.
Its closure records actual commands, fixed SHAs, additions to the original test names, dependencies,
header deletions, byte comparisons and all first failures. Historical receipts are unchanged.
The frozen `evidence/` contains 2,251 files and 406 command records, including development and
failed attempts at their original SHAs. Manifest SHA256:
`0995add672df7eefffc97d86539ad19931a684f23c00eb43b5dcec9d3e23fbb7`.
Relocation to a Chinese/space path, required-file removal rejection and tamper rejection pass.

Six protected user differences remain uncommitted and unchanged. The ProjectBuilder patch stays
independent and unapplied. Public-header synchronization includes Android headers only; no Android
build is claimed. Main and historical branches are untouched.

Graph-editor rendering and remaining metadata-aware source adapters still require work. Later MA
stages remain open. Linux remains unmet / LR08 PARTIAL; native input remains user-deferred.
IME, sanitizer, historical performance, host minimize, skinned WAR and Material VERTEX_COLOR
findings retain their original scope and SHA. This receipt does not close them.
