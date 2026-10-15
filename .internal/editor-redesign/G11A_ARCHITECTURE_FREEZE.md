# G11A-0 architecture freeze

Input: `9a2154371018b082dc2c0270a2c898559d69e213`. This document freezes implementation decisions;
it is NOT a claim that G11A-1 through G11A-5 have passed. Only G11A is authorized.
Package SHA256: `2c7ba8aed1fc886c4af4da0a2f09786d8d1cc4cb7d5f7ae4b10208279072f11a`; six manifest entries verified.
Worktree: `E:/RenderV2Rebuild/G11A/source`. Raw evidence: `E:/RenderV2Rebuild/G11A/evidence`.
`preflight.json` captures the exact baseline tree, all 719 frozen blob identities, dependencies and user hashes.
Git clean-filter hash and literal blob hash are distinguished (Windows checkout EOL conversion is not a source change).
The primary dirty worktree is never used as build input or SDK source.

## Actual source audit and reuse

| Source | Actual fact / decision |
|---|---|
| Native `Device.cpp::selectQueues` | Three roles, dedicated-family preference, same-family distinct indices, alias fallback. Reuse selection, query/enable only required features at device creation. |
| `DeviceSession.cpp::create` | VkQueue equality already deduplicates SubmissionQueue AND RetirementQueue owners. Preserve. |
| `Submission.cpp` | Ticket includes queue owner; timeline waits validate device, stage mask, admitted serial and reject duplicate owner waits. Keep this algorithm. |
| Runtime `Request.cpp`, `Deferred.hpp`, `RenderServer.cpp` | Reserve saves graphics frontier; commit and poll use graphics only. Add an explicit owner-queue qualification, not a numeric global serial. Fixed Deferred slots remain the only source Request owners. |
| `Memory.cpp` / Permit / Retirement | Session alone owns VMA and generational allocation records. Add queue-aware completed release and a narrow mapped-slice/flush boundary for CPU preparation. No resource RAII duplication. |
| Native `Compile.cpp`, `Shader.cpp`, `Execution.cpp` | Current RGBA8 transient image and ABI1 push-color reject imported sampled images. Extend this same compiler/reflection/recipe path; do not bypass it in tests. |
| G4 Graph/Analysis, G5 Feature/Resources | Existing logical access/version/ownership and authored contributions remain authoritative. Only add orthogonal SAMPLED read validation and typed external texture binding. |
| Legacy `StagingRingBuffer.cpp` | Reuse checked alignment/remaining-capacity arithmetic. Do not reuse frame-index reset as GPU completion evidence. |
| Legacy `TransferScheduler.cpp` | Reuse per-image/subresource transition deduplication and format-aware copy planning. Reject unbounded dedicated-buffer fallback and contributor dispatch. |
| Legacy `GpuTransferPipeline.cpp` | Keep release/acquire plus actual semaphore ordering concepts. Do not copy its worker, queue mutexes, jobs/results or second timeline owner. |
| Legacy `BindlessCombinedSet.cpp` | Keep replacement backing and deferred descriptor-index reuse. Do not copy independent generation arrays, early publication or synchronous one-time submission. |
| Legacy `TextureResourceOperation.hpp` | Reuse last-row byte-range reasoning, mip-chain bounds, compressed block math and checked sums, expressed using existing rdesc formats and G2 fields. No native-layout wire ABI. |
| `rdesc/Texture.hpp`, `asset/texture/TextureAsset.cpp` | Pixel-format and image-format enums differ. TextureAssetInfo is persisted POD; keep values, size and field order. No new asset codec or MaterialGraph. |
| `ShaderEmitter.cpp::SAMPLE_TEXTURE`, TextureCooker | Existing material emitter delegates sampling to luxSampleTexture; cooker preserves source color metadata. No proven wide-gamut primaries in old LINEAR assets. Define legacy LINEAR as BT.709/D65 source. |
| gapi Image/View/Sampler | Handle wrappers call macro-based creation and do not return the required structured native failures. Retain existing Native failure/instrumentation path instead of adding gapi linkage. Swapchain stays read-only. |

## Device evidence and synchronization decision

`vulkaninfo.txt`: NVIDIA RTX 4070 Laptop: family 0 graphics/compute/transfer, 16 queues, 64 timestamp bits;
family 1 transfer-only, 2 queues, 64 timestamp bits; family 2 compute/transfer, 8 queues.
Timeline, sampled-image update-after-bind, update-unused-while-pending, partially-bound and runtime arrays supported.
Intel adapter facts are retained separately; NVIDIA is the qualification adapter.
Existing EOS duplicate-overlay warning is retained, not hidden.

Test three explicit configurations: all roles alias; multiple queues from family 0; dedicated transfer family 1.
Only hosts enabling the real Texture consumer request transfer capabilities; old G10 default remains valid.
Use EXCLUSIVE images and paired release/acquire with a real SubmissionWait when families differ.
Same-family different queues still wait; same physical queue never waits on itself.
Finalize into Graphics shader-read readiness before asset publication: a real graphics finalize ticket dominates
the transfer ticket by the actual timeline wait. Subsequent graphics consumers use the existing queue order and
prepared shader-read barriers. Failure before finalize retains the transfer requirement; failure after finalize
retains its graphics requirement. Never compare serials across physical queues.

## Owners, bounded progress and commit points

DeviceSession owns one TextureStorage, physical allocations, all queue owners and retirement. TextureStorage
alone allocates TextureId with lux-cxx SlotMap; IDs are business identities, never descriptor indices.
Physical versions hold ImagePermit, view/sampler/descriptor position, leases and actual last-use evidence.
An update reserves a new version; old version remains sampleable until existing Context leases and submitted
work finish. Descriptor capacity counts published, candidate and retained versions together.
Explicit global release removes logical lookup; creator Client lifetime is irrelevant.

One upload candidate holds validated source spans, row/block progress, exclusive staging slice, CPU-ready
publication and accepted tickets. Original source stays in the original RenderRequest in ONE Deferred slot.
No Native candidate owns a second request or RequestId map. One bounded staging pool has no oversize fallback.
Each progress visit schedules at most one bounded CPU slice or one Native submit; busy preserves progress.
Host supplies a bounded enqueue/join resource (same shape as compile executor, separately identifiable work).
Worker only writes its stable exclusive mapped slice and release-publishes CPU completion. Render owner
acquire-observes, flushes through VMA, records and submits. Worker never mutates DeviceSession/queues.
Staging cannot be reused before its real copy ticket completes. Partial failure drains accepted requirements
before candidate destruction; it cannot erase a prior accepted ticket or publish a new revision.
Stop joins CPU preparation and compile jobs, drains accepted Native work, settles original requests, then
resets Responder, storage/contexts/device and finally instance. No hidden threads or steady-frame wait-idle.

The G10 single-ticket Reserve/Submit/Commit remains available. Its queue choice is explicit and frontier
is captured for that owner. Texture progress is a narrowly typed alternative in the same fixed Deferred
capacity, with reply capacity funded before GPU acceptance. It is not a user-supplied bool that can assert
GPU success. Only final actual ticket completion plus successful Texture publication authorizes SUCCESS.

## Shader, color and dependency decisions

Keep ABI1 unchanged. Add one explicit sampled ABI: fragment combined sampler array, exact set/binding and
fixed bounded size, reflected via existing SPIRV-Cross/SPIRV-Tools. Real indexing features enabled before
creating the device. Pipeline identity includes ABI/layout and exact stages; texture revision is not PSO identity.
Attach resolves TextureId once to prepared Native version/descriptor; stable frames have no string lookup,
Feature callback, reflection, Graph analysis or allocation. SAMPLED read is declared in the unique Graph.

Working space: scene-linear BT.2020 / D65. See SCENE_WORKING_COLOR_SPACE.md. No HDR display or WSI.
Initial formats: RGBA8_UNORM/ SRGB, BC1_SRGB if queried sampled/copy support, with validated 2D mip chains
and region updates. No implicit mip generation, Cube/3D/sparse residency or reinterpretation of DATA as sRGB.

Dependency: neutral texture value contract uses rdesc + existing SlotKey/G2; Native Texture depends on it;
Server-only open Feature handlers use Native and Runtime; Client-only imports must remain Vulkan-free.
Texture.hpp currently couples TextureInfo to generated resource visibility and the Texture class. If necessary,
move only existing value definitions to a canonical lightweight header, included by Texture.hpp; no aliases,
no enum/layout changes and no linking the resource description/script library into Render.

## Scope and frozen verification

Allowed: affected Native Vulkan and Runtime files/tests; minimal Graph SAMPLED validation; narrow neutral
texture Feature/schema binding; canonical rdesc header split if needed; exact build/export/install consumers;
new G11A documents/checker. G2 general codec, Protocol/IPC kernels, Legacy, historical reports/checkers,
MaterialGraph/ShaderIR, gapi, Editor/ECS and primary user files are read-only.

Gates run in order 0→1→2→3→4→5. Preserve old 62 test names. Release/Debug/ASan all twice and complete CTest,
independent final-I clone with no alternates, real validation/sync validation, installed sampled consumer,
pure Client with Vulkan disabled and SDK relocation. No final PASS before these run.
Fixed performance workload: 1/16/64 MiB, explicit 128 MiB client owner budget, bounded staging chunks,
same work on alias and dedicated queues; raw refusals and tail samples retained. Measure prepare CPU,
render-owner CPU, GPU timestamps where supported, ticket/completion latency, peak residency/staging and
four-view p50/p95/p99 both idle and uploading. Do not call mixed CPU timing GPU bandwidth.
Root TOOLCHAIN BLOCKED and IPC R2 PARTIAL remain historical facts until separately proven otherwise.
Linux/Android/TSan remain NOT_RUN unless actually executed. G11B–G13 remain NOT_STARTED.
