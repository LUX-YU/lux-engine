# F2 — Logical Graph contracts and type responsibilities

Base: `c93f9b0ff35f556d6a95a00c585b51c21ca14b2d`. This document describes implementation semantics; only the independent verification report records qualification results.

## Authoring and compilation

`RenderGraphBuilder::graphics/compute` take distinct `GraphicsShaderReference` / `ComputeShaderReference` values. `transfer/readback` do not accept a Shader reference. All four paths capture the existing generated `PassSchema<T>` and invoke one ordinary `addCapturedPass` implementation. The generic `addPass` remains the same typed capture primitive, with ordinary role/stage validation; it is not a second scheduler or an old R3 adapter. Only capture and Concepts are instantiated per Params type. No Description, Meta, emitter, reflection, or generator ABI changed.

`finish() &&` owns the resource/pass/edge/output/provider declarations. Texture/Buffer descriptions are a closed variant; `kind()` is derived. Builder-scoped handles remain checked for ordinary uses, fallbacks, provider publication and output declaration. No new registry exists. Texture formats still use the unique neutral `rdesc::ETextureFormat` definition.

`compileLogicalGraph(definition, options)` is the sole production compiler. `LogicalGraphPlan` has no compile method or mutable public construction. `diagnoseLogicalGraph` invokes the same analyzer to expose an actual cycle path and deterministic JSON when the fixed Core error payload cannot carry an arbitrary-length path. No ErrorRegistry or global diagnostic state was added.

### C0–C8

| Stage | Actual mechanism |
|---|---|
| C0 | Definition validates identities, roles, kinds, format/aspect, dimensions, checked ranges and explicit edges. WholeResource is an authoring sentinel expanded into the correct typed extent; it is not a stored whole-resource hazard flag. Canonical identity normalizes output/import ranges too. |
| C1 | Unique semantic providers resolve to a concrete resource and explicit producing PassKey. Missing required providers fail; optional sources require a valid typed fallback. Imports declare initialized ranges and optional history semantics; outputs carry typed effect and producer. |
| C2 | Generated field capture retains access/stage/layout facts. Builder validates shader versus graphics/compute/transfer role; Definition validates range/format/kind/attachment contracts. Compiled bindings use a closed role variant, not the giant capture record. |
| C3 | Buffer endpoints and Image aspect/mip/layer boundaries partition logical cells. Every cell has independent imported/writer versions. Overlapping writers require explicit order; reads name a producer, imported version, or an unambiguous ordered version. RAW/WAR/WAW are range-specific. No pass insertion order chooses a version. |
| C4 | Deterministic ready-node selection by canonical pass name; DFS reports the concrete cycle path. Declaration-local IDs remain mapped to original declarations, not a graph-isomorphism cache. |
| C5 | Export/Present/Readback/ExternalWrite roots and validated HostReadback operations retain their data producers. Anti-dependencies and pure order constraints do not make dead work live. `cull_unused=false` is an explicit neutral diagnostic retention option used for the migrated R3 complete-schedule vectors. |
| C6 | Per-version/cell intervals plus aggregate resource lifetimes. Exports extend to the logical terminal position. Unused/culled versions have no interval. Candidate reuse requires compatible transient descriptions and disjoint logical intervals; it is never GPU alias/retirement proof. |
| C7 | Scene rejects View/history/target/dynamic-extent and declared Camera/RenderTime/History/Target inputs. Data-producing scopes may not narrow a wider consumer's scope. Scene sharing compares scene/revision, transitive producer invocation values and relevant import epochs/backings. Three-View CPU grouping is not GPU execution. |
| C8 | Owning readonly input cache identity plus resolved execution facts, ordered nodes, versions, hazards, liveness, imports/exports, conditional choices, lifetimes and diagnostics. One candidate transaction returns a complete value or an error. |

The input cache identity is a derived structural snapshot; the resolved identity contains concrete provider bindings and alternative dependencies. Neither is independently mutable through Plan. Full exact typed equality is authoritative. `fingerprint()` is a diagnostic lookup hint using existing Fnv1a64, never sufficient for a cache hit. `explainLogicalCompatibility` lists differing resource/pass/use fields. Equivalent encodings with extra explicit/transitive edges may miss the cache; no hash collision can cause a false match.

Cold compilation allocates proportional to actual declarations/cells. The current transparent implementation uses reachability matrices (O(P^3)) and boundary cells, not per-byte production enumeration. The independent oracle deliberately uses per-byte/per-mip/per-layer/per-aspect enumeration. This stage establishes cold cost for the complete semantics; it does not claim the old whole-resource compiler performed equivalent work.

## Conditions, local read and import lifetime

An atomic condition group is identified by a stable key. Every pass in a group has the same enabled value in an Invocation. A consumer outside a conditional producer's group must supply a valid initialized import or other producer fallback. Both alternatives are compiled and validated; `resourceFor(pass,use)` selects the actual resource from the current Invocation. A fallback reading the initial version of the same backing is mutually exclusive with the skipped writer, so it does not manufacture a false WAR cycle. Conditional exports need an unconditional output consumer with a proved fallback. No topology rebuild or runtime registry is involved.

Local read requires an explicit input-attachment declaration paired with an equal-range attachment write in the same graphics pass and the neutral `allow_local_read` compile option. Unmarked overlap fails. Ordinary storage ReadWrite consumes an initialized previous version. Sampled feedback cannot masquerade as local read. Native feature support, subpass/barrier realization and native alias safety remain F3/F4 responsibilities.

An import's initialized range is an explicit logical promise. Empty ranges mean no imported initial contents. Invocation bindings require a supplied nonzero ready epoch; history additionally matches the Invocation history epoch. These are values supplied by the external owner, not evidence that a token owns native backing. F4 must resolve owner pins, actual waits, layout/queue ownership and GPU completion. LogicalGraph never dereferences or destroys a native object. Dynamic backing identifiers may change without a new compilation.

`FrameGraphBindings` is a synchronous borrow of named Plan, named import storage and optional named owning `GraphInvocationData`. They must remain alive, unmoved and valid while consumed. Rvalue containers, rvalue Invocation and rvalue Plan are rejected; constructing an already-dangling named span violates the span owner's contract. There is no retained execution owner in F2. Invocation owns scalar bytes and closed sampler/clear values, plus Camera/scene/revision/View/Target/history facts. Changing these values updates actual data, not an equality trick. Frame serial/time remain independent of simulation revision.

## Type responsibility inventory

| Type(s) | Category and responsibility |
|---|---|
| VGraphResourceDescription; GraphResource | Closed descriptor value; only TextureDesc or BufferDesc, no duplicate stored kind |
| WholeResource; VGraphRange | Authoring full-extent sentinel / closed range value; canonical resource accesses always typed and bounded |
| GraphImportContract | Owning logical initialized ranges and history promise; no native owner |
| AutomaticProducer; ImportedProducer; PassProducer; SemanticProducer; VGraphProducer | Closed cold version/producer selection values |
| GraphFallback; AuthoringFallback | Definition-local alternative / Builder-scoped typed alternative; no ownership transfer of resources |
| GraphProvider; GraphOutput; EGraphOutput | Unique semantic production declaration and validated terminal effect values |
| GraphResourceUse | Resource, typed range, access/stage/layout and producer/fallback/local-read facts |
| CapturedFieldBinding | Owning short-lived authoring/schema snapshot; never the final plan binding representation |
| ComputeShaderReference; GraphicsShaderReference | Distinct author intent wrappers; names borrowed only during capture, copied by Builder |
| RenderGraphBuilder | Short-lived mutable declaration builder, consumes into Definition; non-template algorithms own validation |
| RenderGraphDefinition; GraphPass | Owning immutable declarations, including actual default dynamic values; value equality remains honest |
| LogicalCompileOptions | Neutral culling retention and explicit local-read legality inputs only |
| ShaderResourceBinding; SamplerBinding; AttachmentBinding; TransferBinding; VGraphBinding | Closed compiled structural binding alternatives, no irrelevant dynamic fields |
| GraphFieldBinding; LogicalPass | Common schema identity and structural compiled pass data |
| LogicalGraphIdentity | Owning exact structural identity; dynamic invocation values excluded and stored elsewhere |
| EGraphHazard; GraphHazard | Typed dependency reason with concrete range |
| LogicalResourceVersion; GraphResourceLifetime | Writer/import cell version, reader set and logical interval |
| LogicalInputChoice | Compiled conditional source selection, with consumer/use/condition producer and fallback resource |
| LogicalReuseCandidate | Candidate interval relation, expressly not a native allocation proof |
| GraphCompileDiagnostic | Owning error/path/JSON output of the same compiler analysis |
| LogicalGraphPlanData | Internal construction/result aggregate; external callers cannot publish it as a Plan |
| LogicalGraphPlan | Owning readonly compile result, not a compiler/manager/runtime |
| GraphImportBinding; GraphFrameValues | Borrowed invocation backing evidence / independent render serial-time-slot values |
| ColorClearValue; DepthStencilClearValue; VGraphDynamicField | Closed actual per-invocation values; integer color Clear remains F3-PRE-01 OPEN |
| GraphPassInvocation; GraphInvocationData | Caller-owned mutable invocation storage, preallocated before stable binding |
| GraphImportStorage | Concept requires a contiguous sized range of exact GraphImportBinding; positive arrays/vector and negative wrong element/rvalue cases |
| FrameGraphBindings | Validated synchronous non-owning view; never a queued frame or resource owner |
| compileLogicalGraph; diagnoseLogicalGraph; logicalIdentity; explainLogicalCompatibility; makeGraphInvocationData; mayShareSceneInvocation | Stateless algorithms, no second authority or persistent service |

Existing identity tags/StrongId aliases, schema Concepts and author resource wrappers remain at their original responsibilities. No production Vulkan/Transport/Scene/Runtime/Feature type was introduced.

## Test migration and provenance

| Existing obligation | F2 treatment |
|---|---|
| All 99 CTest names | Retained; independent qualification compares the previous test inventory with the new one |
| R3 declarations/kind invalid enum | Descriptor variant makes invalid stored kind unrepresentable; malformed descriptor and forged field kind remain runtime negatives |
| R3 hazard vectors and 300 random graphs | Fixture now states its original ordered-stream constraints explicitly; independent O(P^2 R) reachability/lifetime assertions retained |
| R3 10k bindings, sort, lifetimes, candidate failure | Retained under free compile function and typed ranges; explicit import ready evidence supplied |
| F1 HZB unsupported-scheduling assertion | Upgraded to successful real disjoint-mip logical compilation; out-of-range and implicit feedback rejection retained |
| F1 dynamic Definition equality | Actual values now compare unequal; exact logical identity still matches, and real Invocation bytes/sampler/clear updates are checked |
| F1-FIX / FIX-2 roles, ranges, identity, aspects | Same generated schemas and validation obligations; direct malformed fixtures use the descriptor variant |
| Temporary Plan rejection | Named empty input avoids an unrelated deduction failure; the deleted rvalue-Plan overload remains the cause |

Production provenance: the approved R3/F1 Graph value validation and typed Meta capture are evolved in place. The old whole-resource scheduler is removed, not wrapped. Explicit producer/version contracts and range partitioning follow FINAL 06 rather than preserving R3's implicit ordered-stream selection. Frozen V1 `graph/src/DependencyAnalyzer.cpp` and `vulkan/test/compiler_result.cpp` remain behavior references through the existing cycle/initialization/last-good/lifetime vectors; no frozen source is compiled or changed.

New cases cover ranges, versions/cycle path, partial initialization/history, provider ambiguity/fallback, conditions and same-backing fallback, culling/effects, scope/grouping, feedback, 700 mixed Buffer/Texture graphs, generated authoring and five compilation negatives (including const rvalue containers). Original Meta/glslc/reflection fixtures are still generated by production tools. The million-binding comparison is the same functional workload; cold complete logical compilation gets its own measured baseline.

## Open gates and scope exception

F3-PRE-01 integer RenderTarget Clear types: **OPEN**, not implemented by float Clear values.
F3-PRE-02 Shader include/declaration ordering: **OPEN**, not modified in this phase.
Native RenderGraph: **NOT_IMPLEMENTED**. Products: **EXPECTED_UNAVAILABLE** until F11.
No new GPU behavior is claimed; R4 GPU tests remain regression checks. Installed SDK and Android qualification are not claimed by three-prefix header synchronization.

During implementation the user confirmed that the Transport `Error.hpp` constant alignment change was theirs and explicitly authorized including it in this commit. This is a whitespace-only scope exception; qualification checks all other Transport blobs and strips whitespace to verify this file has no semantic change. The original user's six dirty files are untouched.
