# Logical RenderGraph — R3

`lux::engine::render::render_graph` is a C++20 static component with one compiled
dependency, `render_core`. Public paths are `lux/engine/render/graph/{Definition,Plan,Bindings}.hpp`.
There is no Transport, thread, Vulkan, Scene/ECS, Feature execution or frame loop.
The independent bootstrap builds the component and its tests. Products remain unavailable until R17.

## Definition and compilation

`RenderGraphDefinition::create()` consumes resource/pass/explicit dependency value arrays.
Declarations are owned and accessible only through const views. One-based `GraphResourceId`
and `GraphPassId` refer to declaration positions **within that definition**, not runtime/global
identities. Do not mix IDs from unrelated definitions. No generation or owner registry is implied.
Stable target semantics reuse Core IDs. Target semantics are optional, image-only and unique.

Each pass has at most one use per whole resource, with READ, WRITE or READ_WRITE access and
a logical TRANSFER/SHADER/attachment/VERTEX/INDEX/UNIFORM/PRESENT usage. Multiple roles for one
resource in one pass, subresource ranges and feedback attachments are not supported in R3.
Use READ_WRITE for a combined shader access; it requires an initialized value. Vertex/index/
uniform uses are read-only buffers, attachments are images, PRESENT reads an imported image,
and transfer access is either read or write. Invalid enum values and references return errors.
The backend must later check native format/layout/access compatibility, buffer bounds/alignment,
actual imported readiness and device support. R3 does not invent those GPU contracts.

Use lists and explicit edges are canonicalized; duplicate edges collapse, duplicate uses and
self edges are errors. Resource and pass declaration order is semantic, not canonicalized away.
First, a Kahn sort respects explicit edges (smallest declaration position breaks ready ties).
That order defines successive values of mutable logical resources, preserving V1's explicit-order-first
rule. A read before the first write to a transient resource fails, including READ_WRITE.
An imported resource has an initial value. A later-declared producer must be explicitly ordered first;
the compiler does not infer a forward producer by searching future writers.

Single traversal of ordered uses maintains last writer and intervening readers per resource.
It adds RAW, WAW and WAR edges; read/read uses do not serialize each other. Edges are deduplicated
and the final Kahn sort yields a deterministic plan. Explicit cycles return `kGraphCycle`, never
a partial schedule. No pass culling or condition execution exists: all declared passes, including
empty side-effect passes, remain in the plan.

`CompiledGraphPlan::compile()` returns an owning value with a definition snapshot, pass order,
explicit+hazard edges, import order and optional resource lifetimes. Copies are independent
logical values; moves transfer vectors. Moved-from owning values are for destruction/reassignment.
There are no pointers into a temporary builder. Construction/compilation allocates on the cold path;
ordinary OOM is fatal. Errors use Core's expected and the common structured Error/descriptor mechanism.
No logging or second error registry exists.

Each lifetime is an inclusive pair of **execution positions**, with no interval for unused resources.
Imports also have intervals when used. These are logical uses, not physical allocation, aliasing,
cross-frame synchronization or GPU retirement proofs. R3's plan is the logical part of the three-stage
model; native barriers, queue assignment, physical allocation and record plans remain backend work.

## Topology and frame values

`plan.matches(candidate)` performs exact normalized topology equality on the cold invalidation path.
There is no hash-only cache key, mutable global cache or automatic compiler invocation.
Construct a candidate after actual topology changes; adopt a successful plan, retain the old one
on failure. Resource kind/origin/target semantic, pass set/order, resource access/usage and explicit
edges are topology. An independently constructed identical definition matches. Distinct but
semantically equivalent encodings with extra transitive edges may compare unequal; there is no
expensive graph-isomorphism or transitive-reduction cache.

`FrameGraphBindings::create(plan, values, imports)` validates and borrows an existing plan and
an ordered span of all its declared imports. The plan and span must remain alive and unmoved/
unmodified during consumption. The temporary-plan overload is deleted. The view owns only frame
serial, render time in nanoseconds and frame slot values; they carry no Simulation revision.
Import records contain a logical resource ID, nonzero backend-scoped `GraphBackingId`, and buffer
dynamic offset. Image offsets must be zero. Backing identities must be non-aliasing across logical
imports, and duplicate tokens are rejected; the future consumer owns/validates actual backing
lifetime and must not issue distinct tokens for aliased memory without an explicit alias contract.
Graph never dereferences a token or owns native storage. Resources must match `plan.imports()` order.

No new PROGRAM/Simulation input is needed to create successive bindings using the same plan.
Frame serial/time/slot, backing token and dynamic offset may change without a compile or topology
comparison. CPU/GPU frames, camera schema, external GPU waits/signals, format/extent and native
backing resolution are not fabricated here; the appropriate real consumers extend those facts later.
Binding validation performs no allocation or compile, O(imports squared) for alias-token rejection;
the measured R3 case has one import and does not claim a whole-renderer zero-allocation result.

## Frozen V1 sources and test vectors

Reference: frozen SHA `a669409a289a6fa4092f21176397795b1cdb7f3e`, under `render_legacy/` (read-only).

| Source under frozen `modules/function/render/` | R3 adaptation |
| --- | --- |
| `graph/src/DependencyAnalyzer.cpp` | Explicit-order-first, resource-use indexing, RAW/WAW, deterministic Kahn, first/last uses; own neutral values, no owner hierarchy. WAR is explicitly covered rather than inferred from V1's implementation. |
| `vulkan/test/compiler_result.cpp`: two-pass cycle | `legacy` rejects `kGraphCycle`. |
| Same: required reference with no producer | `legacy` rejects transient read/read-write without producer. R3 uses ID references rather than name-based forward-reference objects. |
| Same: missing imported native source | `legacy`/`bindings` reject missing/zero bindings at frame binding time; topology compilation has no native source. |
| Same: retained empty side-effect pass and moved owned graph | `legacy` preserves all passes and checks owned moved plan. |
| Same: failed replacement retains last good graph | `legacy` compiles a cyclic candidate and verifies the existing plan remains untouched. No SceneGraphCache is copied. |
| Same: pipeline, queue transfer, conditional attachment/clear, formats, callback lifetime | Deferred to actual Vulkan/Feature consumers; not counted as R3 GPU tests. |

No separate V1 Graph test target was present in the frozen `graph/` directory; applicable compiler
vectors above are translated into CPU-only contracts, not a claim of running the old GPU suite.
New `hazards`/`lifetimes` cases cover explicit reordering, multiple readers, imported WAR and unused
resources. `generated` uses 300 fixed-seed graphs (24 passes, 7 resources) and an independent
pairwise conflict/reachability and lifetime oracle. `reuse` varies 10,000 frame bindings while
retaining the same plan, then checks topology changes require a new candidate.

Each public header compiles independently. Real-target negative consumers cover IDs, temporary
plan borrows and forbidden dependency includes, with a positive control. `verify_r3.py` audits
actual CMake targets, includes, source/link closure, Ninja compiler dependencies and unchanged
Transport codegen. The normal benchmark records one million bindings and C++ allocation counts;
test instrumentation does not alter the production library.

The existing generic error-header installed SDK closure remains deferred to R16/R17. Three-prefix
header synchronization is not installed SDK qualification. R3 completion requires separate clean
commit qualification and STOP; R4 is not authorized by this README.
