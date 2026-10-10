# F2-FIX — Algorithm and public contract inventory

This supplements the previous F2 contract description without changing historical qualification. Scope correctness, not source line count or cold allocation count, is the repair's blocking issue.

## Three relations, one edge authority

The edge insertion path still records `GraphHazard` values and the execution adjacency matrix. A separate `data_liveness` relation includes only RAW/FALLBACK. C5 propagates roots backward with a bounded worklist; each pass enters once. ORDER/WAR/WAW do not retain otherwise dead work.

C7 derives `share_preconditions` from **all execution edges with live endpoints**, closes that relation, and grants eligibility only after it proves predecessor scopes. Conditional alternatives have already been validated by unchanged C1/C3. Their union is conservative: a potentially required narrow predecessor rejects a Scene consumer even if a particular invocation disables that branch. A culled predecessor neither grants evidence nor becomes live because it was ordered before a root.

Explicit execution rule: Scene predecessors may precede Scene/View/Target; View predecessors may precede View/Target; Target predecessors may precede Target. This is a declared logical policy, not arithmetic on enum ordinals. A conflicting live path returns `kGraphScopeConflict`; error numeric parameters identify destination/source PassKeys. Diagnostic path keys, names and concrete edge reasons permit inspection. Existing intrinsic Scene/View resource/input restrictions remain separate.

Every live Scene pass's `sceneSources()` includes itself and all live transitive predecessors, not only data producers. Existing `mayShareSceneInvocation()` compares their actual scalar/sampler/clear/enabled values and imports, plus Scene/revision. Thus a Scene ORDER predecessor's changed Params, backing, ready epoch or condition prevents sharing its successor. Camera/View alone do not prevent sharing truly Scene-only work. This remains logical eligibility, not a native GPU execution proof.

## Responsibilities

| Type/function | Responsibility; ownership/borrow/error/cost |
|---|---|
| `GraphCompileDiagnostic` added `scope_path`, `scope_names`, `scope_edges` | Owning cold diagnostic values; no borrowed strings or execution owner. Produced by the same analyzer used by compile. Error path includes actual hazard edges. |
| `LogicalGraphPlan::diagnosticDigest()` | Renamed diagnostic presentation digest. Fnv1a64 is reused. **Not a structural hash or cache lookup identity**. No old `fingerprint()` alias. |
| `cacheIdentity()` / `matches()` | Unchanged complete exact typed compatibility. Structural hashing is not supplied in this phase; future cache indexing must define it without using diagnostic digest. Native cache identity remains F3/F4+ responsibility. |
| `isShaderDescriptorRole()` | Unique neutral constexpr classification in PassContract; used by Graph Builder and existing Shader validator. Closed switch, invalid enums false; no enum order assumption. |
| `isPassFieldRole()` | Unique neutral validity predicate extending descriptor roles with explicit nonshader roles. Used by the generated Schema Concept check; no metadata ABI/layout change. |
| `closure()` | Private cold word-parallel Warshall relation calculation. Temporary packed words reduce inner work; output relation is identical. No resource-version policy changes. |
| `findCycle()` | Extracted ordinary DFS from the existing coordinator, returns the same cycle diagnostic; borrows graph/edges, owns temporary color/path values. |
| `retainDataProducers()` | Private backward liveness worklist over producer relation; mutates cold result liveness only. No synchronization edge promotion. |
| `scheduleLiveGraph()` | Extracted deterministic ready-node scheduling with original canonical-name tie-break and dependencies. Does not grant sharing eligibility. |
| `computeLogicalLifetimes()` | Extracted original version intervals and transient candidate calculation; native alias/retirement remains unproved. |
| `permitsExecutionPredecessor()` | Private explicit Scope policy predicate, no state or allocation. |
| `describeScopePath()` | Cold breadth-first path reconstruction only on conflict; owning path and concrete hazard reasons. The caller has already proved reachability. |
| `proveScopeSharing()` | Private free algorithm over live execution constraints; closes share prerequisites and produces complete eligibility/source lists or precise error. Scratch lives only during compile. |
| `formatPlanDiagnostics()` | Extracted deterministic diagnostic JSON formatting; no structural cache authority. |
| `analyze()` | Sole short-lived coordination path; C1/C2 and C3 version/hazard capture stay in place. Compile and diagnose still share it. No CompilerManager or second scheduler. |
| ScopeProof / ScaleBenchmark | Test-only CPU fixtures and cold measurement. Windows process memory API is confined to the benchmark executable; production Graph has no OS/native dependency. |

These are plain values/free functions because none has independent lifecycle, identity or a persistent service responsibility. No new production template, owner, parser, thread, Feature or Runtime was added. Hot Binding production code is unchanged.

## Regression and performance evidence obligations

ScopeProof covers real-root ORDER, WAR/WAW/RAW, transitive chains in reverse declaration order, explicit Scope combinations, imported/parameter/revision prerequisites, condition enabled/disabled paths, dead ORDER culling and full diagnostic keys/edges. Two definitions with identical diagnostic digest but different Shader interface or Texture format must fail exact compatibility. Every existing role and invalid enum values test the shared classifiers.

The 116 earlier CTest names and original 300 random graphs/10k bindings plus 700 mixed oracle remain. Existing generated Shader fixtures and negative Concepts continue through the real generator/glslc/reflection chain. Before-fix reproduction uses the approved F2 library; after uses the repaired production path, with no special production test branch.

Cold scales use 64/128/256/512 passes, 16 resources split between Image and Buffer, increasing mip/layer/subrange cells, explicit versions, culling and diagnostic generation. Raw samples, total first-party allocation calls/bytes and OS peak process working set are recorded; the latter includes runtime/fixture/measurement overhead and is not exact compiler live heap. The unchanged 64-pass/16-resource old cold fixture remains separately measured; different mixed workloads are not conflated with it. Large samples use fewer repeats and disclose that fact.

Word-parallel closure and worklist liveness preserve edge/version semantics and are checked against the existing independent oracles and the same scale input before/after. Binding retains seven alternating pairs of 1,000,000 operations, 10k warmup and fixed affinity; no sample deletion. Qualification, not this inventory, determines performance gate status.

F3-PRE-01 integer Clear and F3-PRE-02 Shader include/declaration ordering: **OPEN**. Diagnostic digest is deliberately not a structural cache hash; future native/cache implementation must supply its own complete identity. No native Graph or new GPU result is claimed.
