# Render V2 cumulative capability evidence view

Snapshot baseline: `c986e69765c9dd222469d4cf5090aa72549966dc`. F3 is **IN_PROGRESS / NOT_QUALIFIED**.
This is the single cumulative index requested during F3. Retain this path in later authorized stages.
FINAL CSVs and historical reports remain read-only. The rows below are references, not a second design.

## Reading the status

- `OPEN`: the whole FINAL row has not been closed by this index; cited sub-evidence is not whole-row PASS.
- `F3_PENDING`: this phase owes independent evidence. Development builds are not qualification.
- Evidence classes distinguish DOC, AUTHOR/SHADER, LOGICAL_CPU, NATIVE_GPU, SANITIZER and PRODUCT/SDK.
- Historical R4 GPU tests are regressions only; they do not qualify F3 pipelines or Native RenderGraph.
- A later stage may close a row only with every required category and any cross-stage obligation complete.
- For a report added by V, resolve V with `git log -1 --format=%H -- docs/render-v2/<report>` and read I from
  that immutable report. The final F3 report is absent until qualification. Do not insert a guessed/self-referential SHA.
- No implementation-completion percentage is computed from partial mechanism fixtures.

## Qualified historical evidence anchors

These are report assertions at exact locked commits, not a new re-run or independent human review.

| Phase | Implementation I | Verification V | Category and limits |
|---|---|---|---|
| F0 | `1eab572630a3a44546719238cc96795cedb2d556` | `00ba4075ada74f711fd08300b5f45f2f4ce2150f` | [DOC: package/freeze qualification](F0_VERIFICATION.md) |
| F1 | `78a1248d1f5e9c59eb8f88ddd4869244e512050f` | `877d44c6eaea03dd5746b03f67af158e97a8ee71` | [AUTHOR/SHADER: original qualification and its limitations](F1_VERIFICATION.md) |
| F1-FIX | `825cce0bf228af32e6c3acd330a8a6f518cc951b` | `bf922e84235487dcdddf0c8a8c1c75ac29abadf8` | [AUTHOR/SHADER: format/range/stages/identity corrections](F1_FIX_VERIFICATION.md) |
| F1-FIX-2 | `905efe1306a9c427bf8a121a0470f385d3c0cf07` | `c93f9b0ff35f556d6a95a00c585b51c21ca14b2d` | [AUTHOR: kind/local identity/aspects; normal/full ASan](F1_FIX_2_VERIFICATION.md) |
| F2 | `f2318d708609e686ed49bdf61de1bade9b17279e` | `3216bc8d19567abf8fc00c76bab7fdb3fff77e1a` | [LOGICAL_CPU: C0-C8; subject to F2-FIX scope correction](F2_VERIFICATION.md) |
| F2-FIX | `946cf0866eda3566abc2fe2d06581ff868e157c3` | `c986e69765c9dd222469d4cf5090aa72549966dc` | [LOGICAL_CPU: corrected sharing proof; normal/full ASan 124/124](F2_FIX_VERIFICATION.md) |

## Capability coverage (all 86 frozen IDs)

Whole-row OPEN is deliberate where runtime, native, installation or later business evidence is still owed.

| ID | Frozen capability | Phase | Current evidence / status | Remaining obligation |
|---|---|---|---|---|
| F01 | MeshStack instance/transform/visibility/retire | F8 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F02 | Forward GPU-driven mesh | F8 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F03 | Deferred GBuffer GPU-driven mesh | F8 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F04 | Deferred clustered lighting | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F05 | Light scene data provider | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F06 | StandardMaterial / graph materials | F8 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F07 | Depth prepass | F8 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F08 | ShadowMap PCF/EVSM/CSM | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F09 | Mesh shadow GPU-driven casting | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F10 | HZB mip build and view history | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F11 | Spatial cell coarse culling | F8 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F12 | GPU pre-skinning / bone palette | F8 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F13 | RenderCluster hierarchy / GPU cull / raster picking | F8 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F14 | Standard per-View camera | F8 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F15 | Tonemap / exposure | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F16 | SSAO | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F17 | Linear Depth | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F18 | Fog | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F19 | Highlight mask blur composite | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F20 | Skybox cubemap/equirect | F9 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F21 | Grid 2D | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F22 | Grid 3D | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F23 | Gizmo transient line | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F24 | Gizmo transient triangle | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F25 | Canvas2D image/pixel field/tilemap/grouping | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F26 | Terrain page stream/LOD/fallback | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F27 | Water surfaces | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F28 | Trajectory lines | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F29 | Streaming visual feedback | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F30 | PointCloud Simple: CPU-fed direct point cloud | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F31 | PointCloud GPUDriven: compute cull/indirect GPU-driven points | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F32 | PointCloud LOD: point size/depth LOD octree | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F33 | PointCloud Splatting: Gaussian soft-splat point cloud (not full 3DGS) | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F34 | PointCloud Transient: per-frame transient points | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F35 | GPU point resource storage & octree | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| F36 | Scene-level trajectory global buffer | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I01 | Fluent graph authoring/Pass builder | F1 | OPEN; AUTHOR/SHADER sub-evidence: F1 → F1-FIX-2 reports above | Reconcile complete row requirements; native/integration evidence is not implied |
| I02 | Logical hazard analyzer | F2 | OPEN; LOGICAL_CPU sub-evidence: F2 + F2-FIX reports above | Exact full-row closure review; Native Graph is NOT_IMPLEMENTED |
| I03 | Native graph compilation/barriers | F4 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I04 | Graph recorder and executable program | F4 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I05 | Physical transient allocator/aliasing | F4 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I06 | Transactional graph cache | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I07 | Generated PassParams HPP | F1 | OPEN; AUTHOR/SHADER sub-evidence: F1 → F1-FIX-2 reports above | Reconcile complete row requirements; native/integration evidence is not implied |
| I08 | Generated PassParams GLSL | F1 | OPEN; AUTHOR/SHADER sub-evidence: F1 → F1-FIX-2 reports above | Reconcile complete row requirements; native/integration evidence is not implied |
| I09 | Layout plan/descriptor layout | F3 | F3_PENDING; no independent F3 I/V yet | F3 normal/ASan/native pipeline evidence and exact I/V |
| I10 | SPIR-V binding relocation | F3 | F3_PENDING; no independent F3 I/V yet | F3 normal/ASan/native pipeline evidence and exact I/V |
| I11 | Engine shared descriptor set shapes | F3 | F3_PENDING; no independent F3 I/V yet | F3 normal/ASan/native pipeline evidence and exact I/V |
| I12 | Shader resource catalog and variants | F3 | F3_PENDING; no independent F3 I/V yet | F3 normal/ASan/native pipeline evidence and exact I/V |
| I13 | Staging/native transfer | F4 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I14 | GPU deferred destruction | F4 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I15 | Runtime owner/lifecycle/reply pump | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I16 | Frame Driver GPU fences | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I17 | Frame Orchestration/target layers | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I18 | Multi scene/multi view Renderer | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I19 | Surface/offscreen target registry | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I20 | Scene RenderSystem/transaction | F7 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I21 | Asset request and resource dedupe | F7 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I22 | Scene resource/view receipts | F7 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I23 | View camera extraction | F7 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I24 | Feature-centric old sync stage replacement | F7 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I25 | UI graphics feature and compositing | F10 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I26 | External feature SDK sample | F6 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I27 | EngineRendering product integration | F11 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I28 | GLSL canonical resource emitter | F1 | OPEN; AUTHOR/SHADER sub-evidence: F1 → F1-FIX-2 reports above | Reconcile complete row requirements; native/integration evidence is not implied |
| I29 | SPIR-V reflection | F1 | OPEN; AUTHOR/SHADER sub-evidence: F1 → F1-FIX-2 reports above | Reconcile complete row requirements; native/integration evidence is not implied |
| I30 | Logical LayoutContract | F1 | OPEN; AUTHOR/SHADER sub-evidence: F1 → F1-FIX-2 reports above | Reconcile complete row requirements; native/integration evidence is not implied |
| I31 | Per-view shared graph plan and native key | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I32 | Pass-scope verified scene work sharing | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I33 | Vulkan Multiview and fallback | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I34 | Subresource mip/layer and buffer range exact hazards | F2 | OPEN; LOGICAL_CPU sub-evidence: F2 + F2-FIX reports above | Exact full-row closure review; Native Graph is NOT_IMPLEMENTED |
| I35 | Actual multi-queue Sync2 and ownership fallback | F4 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I36 | History and conditional pass safe fallback | F4 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I37 | Local read and attachment load/store resolve | F4 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I38 | C++20 PassSchema concepts and generated optional ops | F1 | OPEN; AUTHOR/SHADER sub-evidence: F1 → F1-FIX-2 reports above | Reconcile complete row requirements; native/integration evidence is not implied |
| I39 | Unsolicited diagnostic port separate from replies | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I40 | Render projection retained revision backpressure | F7 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I41 | Plugin code pin native graph in-flight retirement | F6 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I42 | Shader cache/material last-good hot reload | F3 | F3_PENDING; no independent F3 I/V yet | F3 Shader/PSO cache and last-good; F8 registered Material hot update; do not close whole row in F3 |
| I43 | Multi target scene-view layer composition | F5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| I44 | Record hot path no lazy pipeline or heap | F4 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| H01 | Virtual Geometry / Nanite-like | F11+H1 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| H02 | Dynamic GI and Reflections / Lumen-like | H2 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| H03 | Temporal Upscaling / TSR-like | H3 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| H04 | Virtual Shadow Maps / VSM-like | H4 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| H05 | Advanced BSDF / Volumetric and multiview quality | H5 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |
| H06 | 3DGS and mixed representations | H6 | OPEN; no row qualification recorded | Full frozen acceptance in its authorized phase |

## Golden workloads (all 25 frozen IDs)

| ID | Frozen workload | Phase | Status / evidence boundary |
|---|---|---|---|
| W01 | Tonemap+Generated Blur+Composite | F3 | F3_PENDING; Tonemap + generated Blur + Composite GPU and CPU image references; no generic recorder |
| W02 | HZB mip chain | F4 | OPEN; full workload not qualified |
| W03 | Compute-Graphics-Readback | F4 | OPEN; full workload not qualified |
| W04 | Transient alias/noalias | F4 | OPEN; full workload not qualified |
| W05 | N Views same RenderPath | F5 | OPEN; full workload not qualified |
| W06 | 2 Scenes shared asset | F7 | OPEN; full workload not qualified |
| W07 | Skinning 3 FIF | F8 | OPEN; full workload not qualified |
| W08 | Shadow PCF EVSM CSM | F9 | OPEN; full workload not qualified |
| W09 | Terrain streaming fallback | F10 | OPEN; full workload not qualified |
| W10 | Five PointCloud implementations | F10 | OPEN; full workload not qualified |
| W11 | Canvas+UI+Picking | F10 | OPEN; full workload not qualified |
| W12 | External Plugin SDK code pin | F6 | OPEN; full workload not qualified |
| W13 | 10Hz Simulation 144Hz Render | F5 | OPEN; full workload not qualified |
| W14 | Resize minimize lost | F5 | OPEN; full workload not qualified |
| W15 | Shared descriptor full layout | F3 | F3_PENDING; complete shared owner shapes across real VS/FS pipelines and GPU output |
| W16 | Multiview and separate fallback | F4 | OPEN; full workload not qualified |
| W17 | Create update update delete under backpressure | F7 | OPEN; full workload not qualified |
| W18 | Modern C++20 compile rejects | F1 | OPEN; F1/FIX/FIX-2 AUTHOR/SHADER positive/negative evidence above; complete row closure requires review |
| W19 | Full Legacy parity/product | F11 | OPEN; full workload not qualified |
| W20 | Virtual geometry | H1 | OPEN; full workload not qualified |
| W21 | Dynamic GI/reflections | H2 | OPEN; full workload not qualified |
| W22 | Temporal upscaling | H3 | OPEN; full workload not qualified |
| W23 | Virtual shadow maps | H4 | OPEN; full workload not qualified |
| W24 | Layered material/volumetric | H5 | OPEN; full workload not qualified |
| W25 | 3D Gaussian Splatting mixed rendering | H6 | OPEN; full workload not qualified |

## Requirement cross-index (all 51 frozen IDs)

Workloads link back to the same evidence above. No global HARD GATE is discharged by a CPU-only fixture.

| ID | Requirement | Phase | Workloads | Whole requirement |
|---|---|---|---|---|
| REQ01 | One runtime/backend/frame loop | F5 | W13,W14 | OPEN |
| REQ02 | View does not own graph by default | F5 | W05 | OPEN |
| REQ03 | Same path views plan reuse | F5 | W05,W16 | OPEN |
| REQ04 | Scene shared GPU works once only when proven | F5/F8 | W05,W07 | OPEN |
| REQ05 | Multiview optional and separate fallback | F5 | W16 | OPEN |
| REQ06 | Independent Sim/Publication/Render | F5/F7 | W13 | OPEN |
| REQ07 | Three transport lanes without global order | R2/F7 | W17 | OPEN |
| REQ08 | Reply consume or abandon pre-reserved slot | R2 preserve | Existing R2-FIX CTest | OPEN |
| REQ09 | Revision retained on backpressure | F7 | W17 | OPEN |
| REQ10 | AssetDomain shared across Scenes, no GPU double owner | F7 | W06 | OPEN |
| REQ11 | RenderData Capability GraphResource orthogonal | F1/F7 | W17,W18 | OPEN |
| REQ12 | GraphBuilder typed PassParams interface | F1 | W01,W18 | OPEN |
| REQ13 | Pass schema same source C++ Shader Graph | F1/F3 | W01,W15 | OPEN |
| REQ14 | Shader metadata/refl/LayoutPlan full shape and relocation | F3 | W01,W15 | OPEN |
| REQ15 | No manual author shader set binding | F1/F3 | W01,W12 | OPEN |
| REQ16 | Per-mip/layer/buffer range logical hazards | F2 | W02 | OPEN |
| REQ17 | Versioned RAW WAW WAR and init proof | F2 | W02 | OPEN |
| REQ18 | Complete logical Graph compile and culling/conditional | F2 | W02,W04 | OPEN |
| REQ19 | Native plan distinct shader/pipeline/device key | F3/F4 | W05,W15 | OPEN |
| REQ20 | Queue sync2 and real async fallback | F4 | W03 | OPEN |
| REQ21 | Transient alias based on GPU overlap | F4 | W04 | OPEN |
| REQ22 | History View owned, resize invalidation | F4/F5 | W02,W14 | OPEN |
| REQ23 | Target color depth/MSAA local read present | F4/F5 | W11,W14 | OPEN |
| REQ24 | R4 RAII owner single lifetime | R4 preserve/F4 | W03,W14 | OPEN |
| REQ25 | Feature concrete and optional Concepts hooks | F6 | W12,W18 | OPEN |
| REQ26 | Plugin installed SDK and code pin unload | F6 | W12 | OPEN |
| REQ27 | Mesh GPUdriven material variants | F8 | W07,W19 | OPEN |
| REQ28 | Light PBR Forward Deferred Clustered | F9 | W08,W19 | OPEN |
| REQ29 | PCF EVSM CSM MeshShadow | F9 | W08,W19 | OPEN |
| REQ30 | Hzb cull Skinning WAR | F8/F9 | W02,W07 | OPEN |
| REQ31 | Terrain/Water/Canvas/5point/Trajectory/RenderCluster | F10 | W09,W10,W11,W19 | OPEN |
| REQ32 | UE virtual geometry algorithm | H1 | W20 | OPEN |
| REQ33 | UE dynamic GI reflections algorithm | H2 | W21 | OPEN |
| REQ34 | UE temporal superresolution algorithm | H3 | W22 | OPEN |
| REQ35 | UE virtual shadow mapping algorithm | H4 | W23 | OPEN |
| REQ36 | UE advanced material/volumetric algorithm | H5 | W24 | OPEN |
| REQ37 | C++20-only no C++23, value/algorithm/RAII | all | W18 | OPEN |
| REQ38 | Zero first-party warm frame allocations | F4 onward | W13,W19 | OPEN |
| REQ39 | Stop/terminal/device lost fence proof | F5 | W14 | OPEN |
| REQ40 | Independent clean clone I/V STOP | all | All Workloads | OPEN |
| REQ41 | F0 docs authority no historical report rewrites | F0 | F0 doc integration | OPEN |
| REQ42 | Legacy frozen 719 blobs and complete parity | all/F11 | W19 | OPEN |
| REQ43 | Avoid fake GPU and placeholder unsupported claim | all | W03,W19 | OPEN |
| REQ44 | No second parser/Shader metadata registry | F1 | W01,W15 | OPEN |
| REQ45 | External Feature cannot include backend private | F6 | W12 | OPEN |
| REQ46 | Device capability fallback correct and explicit | F4 onward | W03,W16,W20 | OPEN |
| REQ47 | Runtime diagnostics separate from reply | F5 | W14,W17 | OPEN |
| REQ48 | No hidden per Feature SceneSystem/FrameLoop | F6/F7 | W06,W12 | OPEN |
| REQ49 | Plugin ABI cross DLL no leaking owning STL | F6 | W12 | OPEN |
| REQ50 | R0–R4 historical PASS/NOT_RUN preserved | F0 | F0 evidence snapshot | OPEN |
| REQ51 | 3DGS same Scene/View/Graph mixed without separate renderer | H6 | W25 | OPEN |

## Current limitations

F3-PRE-01/02 have development author/emitter tests, not final F3 qualification; remain OPEN until final evidence.
G01–G07 are native output/lifetime obligations; G08/G09 are cold negative obligations with actual device/binary facts.
NATIVE_RENDER_GRAPH = NOT_IMPLEMENTED. RENDER_RUNTIME = NOT_IMPLEMENTED.
V2_PRODUCT = EXPECTED_UNAVAILABLE. F4 and later remain NOT_AUTHORIZED.
