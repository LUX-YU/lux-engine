# G11A implementation gates

Baseline: 9a2154371018b082dc2c0270a2c898559d69e213. This is a construction log, not final I/V qualification.

## G11A-0

Package six SHA256 entries and ZIP CRC checked; isolated worktree; Legacy719 blobs and two user SHA256
values match. Native/Runtime/Graph/Feature/Legacy/resource-color source audit recorded in architecture freeze.
Actual NVIDIA families support alias, same-family separate queues and transfer-family EXCLUSIVE testing.

## G11A-1

Queue-aware G10 reserve captures one explicit Session queue's frontier; commit validates that owner and
strictly newer admitted ticket; poll uses that same owner. The old default graphics overload is preserved.
Session completed-release accepts only one of its actual queue owners; image and buffer permits retain their
generation/retired proof. Aliased roles still share one SubmissionQueue and one RetirementQueue.

Release `all -j 4 -- -k 0` succeeded, second all no-work. First complete 65-test execution preserved old 62
and passed Native transfer; two new Runtime variants failed in the old saturation fixture. The old test
assumed four Graphics Views filled the upload queue. A separate Transfer queue remained free, accepted a
request, and its deliberately held completion could never reach the expected capacity failure. Corrected
fixture fills that explicit physical queue with bounded real submissions, without changing production queue
logic, sleeps, timeout or capacity assertion. Both new variants then pass (22.62 / 28.45 seconds).
Original failure log: evidence/a1-first-ctest.log. Successful targeted log is retained by run.ps1.

Native transfer: alias 0:0→0:0; same-family 0:2→0:0; dedicated 1:0→0:0. Actual Image Copy,
EXCLUSIVE release/acquire, timeline wait and readback verify 1024 bytes per case; validation_errors=0,
warnings=1 (existing overlay warning). Runtime transfer tests preserve two Clients, four Views, original
Owner, failure/stop and 1000 complete GPU-ready operation samples. Dedicated sample: 499 Upload completions
while Main pauses 1.5s; Native buffers 3022 creates/3022 destroys; no GPU validation error.
DeferredCompletion 448→456 bytes (one borrowed queue pointer); Request216/Reservation24/Context40,
Permit16/IpcMessage144/IpcReply296 unchanged. This is not Texture performance qualification.

G11A-4..5 and independent V have not yet run. G11A overall is NOT_COMPLETE; no PASS or push authorized
by these internal results alone. G11B–G13 remain NOT_STARTED.

## G11A-2

One Session-owned fixed staging page array, bounded bytes and one TextureUpload candidate per admitted
upload. Host callbacks copy at most one configured page; only the render owner flushes/submits. Each slice
is reused only after its actual ticket completes. Restrictive transfer-family copy granularity selects the
existing graphics queue rather than emitting unsupported row transfers. There is no dedicated allocation
fallback, production upload thread or second pending request collection.

The existing GpuCompletionReservation admits TextureUpload before first prepare/submit, retaining the
original RenderRequest in the same DeferredCompletion vector. Native advance proves actual final Graphics
completion (including Transfer release/acquire); callback publishes business state once. A failed later
chunk keeps the preceding ticket and source; cancellation advances only toward safe disposal. Shutdown joins
host prepare before GPU drain and before disposal of source-qualified requests. Ordinary GPU-ready reply
remains forbidden. UploadOptions is a dependency-light host contract; RenderServer header still does not
include Vulkan. An initial include leak was caught by the unchanged standalone header gate and corrected.

TextureInfo's existing POD declarations moved verbatim into its own lightweight header, included by the
original Texture.hpp. No enum, field, size or asset codec was changed. RGBA8 UNORM/SRGB and BC1 SRGB
validation accounts for complete mip block rows, exact lengths, overflow, color/format flags and device
format support; BC feature is explicitly requested, not assumed.

Release full all + second no-work and all 67/67 CTests passed, including all original 62 names. Additional
stop-during-CPU-prepare and second-submit DeviceLost fault cases then passed in texture_progress. DeviceLost
is the existing test-only Native call fault hook, not an induced physical GPU reset. Original actual accepted
GPU chunk precedes the failed submit. CPU prepare shutdown uses a latch; source/pin destruction is counted.

Raw samples: build/release/render/native/vulkan/test/texture-chunks.csv and
build/release/render/runtime/test/texture-progress.csv (copied into evidence/a2 before later runs).
1/16/64 MiB use 1/16/64 copy chunks with exactly 1 MiB allocated staging. Two independent queues add one
Graphics acquire submission. Four-mip uncompressed and BC1 readback compares every byte. Dedicated 64 MiB:
4.2214 ms host memcpy, 13.5027 ms cumulative owner work including polling; native test wall 61.6987 ms
INCLUDES image/readback allocation and full readback. Runtime wall 259.5463 ms includes IPC and unchanged
1 ms server sleep across 130 ticks. These are single internal samples, NOT a performance qualification or
GPU timestamp/bandwidth claim. Texture sampling/region/version publication belong to the following gates.

## G11A-3 storage and open Operation checkpoint

DeviceSession owns exactly one TextureStorage. Its SlotMap allocates session-local business TextureId;
its bounded StableSlotMap owns independent physical descriptor versions. These are different lifetimes:
one business identity can temporarily retain old sampled versions. Physical VMA ownership remains in
DeviceSession; TextureVersion holds a move-only ImagePermit plus View/Sampler, completed/last-use ticket,
and Context lease count. Descriptor index is never a business ID. Explicit feature checks enable sampled
image update-after-bind, update-unused-while-pending, partially-bound and array indexing. Descriptor count
is checked against queried Vulkan indexing limits. Reserved unused slots cannot be sampled before publish.

TextureUpload itself holds the publication reservation; no second transaction owner or Request table.
Rollback destroys View/Sampler before its image; successful final Graphics completion atomically moves the
permit and swaps the current version. Failed/duplicate/stale revision writes leave last-good intact.
Release invalidates TextureId immediately but live physical leases/tickets prevent slot reuse.
Region updates clone all old mips into a different image on Graphics, restore the source layout, insert
clone/patch WAW ordering, and pack only the affected padded rows in the host worker. RGBA and BC block
regions have exact overflow/range/pitch checks. Full replacements incompatible with leased graph dimensions,
format/layers/mips are rejected instead of invalidating prepared consumers.

Production neutral UploadTexture/UploadTextureRegion/TexturePublished schemas and the optional
textureFeatureRegistration use the existing open registration path, no Server business switch. Global
creation works without Scene/View/Effect. Completion and fixed reply extent are reserved before allocation;
new identity values replace that same validated reply extent before any submit. Publication consumes the
original DeferredCompletion and TextureUpload, without allocating another callback Owner. A creator Client
can close; another Client can replace/release the Global texture. Same numeric RequestId tests use distinct
physical links. The test initially failed because only A had made a locally rejected submission (which also
advances its request generation). Mirroring that rejected boundary call on B preserves the identity assertion;
the targeted test passes after correction. No production Client/IPC identity algorithm was changed.

Real Native tests cover descriptor layout/pool/set/view/sampler failures, fixed slot exhaustion, exact
destruction counts, stale IDs/revisions, pinned physical generations and padded RGBA/BC mip Region readback.
Validation error count is zero. Full 69-test Release run passed 68, with only the above new fixture failure;
its corrected targeted rerun passed. All prior 62 tests passed. Cross-Scene sampled consumers and GPU color
qualification still require G11A-4; these internal results are not the final G11A PASS.

## G11A-4 sampled consumer

G4 adds only SAMPLED use validation (Image + READ + non-Transfer pass + format usage capability).
Analysis/version/hazard algorithms remain unchanged. Native ABI2 reflects one runtime combined-sampler array
at set 0/binding 0 and the existing 16-byte push block; ABI1 remains byte-for-byte compatible. Cold compilation
requires Imported Global TextureStorage ABI1 metadata and a TextureId. Context preparation resolves it once,
validating dimensions/format/mips. The prepared pointer array is empty for ABI1 graphs. Binding stays 32 B;
Context adds one 24-byte vector. Each accepted Graphics submission stamps the physical versions' true ticket.
Texture publication refreshes active Context borrows at the topology change, never by per-frame ID lookup.
Old versions retain their prior GPU last-use, and retired Context leases remain untouched until retirement.

The real Feature→Assembly→Graph→Compiler→Executor path samples RGBA DATA/SRGB/LINEAR and BC1 SRGB;
no hand-made test Vulkan pipeline. Two Scene/four View contexts share one compiled graph and global image.
Four mips, full replacement, padded Region update and release with attached consumers are read back.
The independent double BT.2020 oracle observed max error 0/255 on this NVIDIA run (contract remains 2/255;
DATA remains exact). Premultiplied LINEAR input uses valid premultiplied RGB and preserves alpha.
Wrong Descriptor binding, ABI, missing identity and invalid LOD are rejected before pipeline publication.
Shader source, deterministic cooker and cooked SPIR-V with hashes are retained under test/shaders.

## G11A-5 integration and measurement

Real Texture Operations and a sampled Feature run through the same open Runtime path. Two cold compiler
workers overlap upload preparation and four Views. Main retains 64 replies for 1.5 seconds; Upload continues
16 MiB transfers while Graphics progresses. Creator disconnect does not release Global assets. Existing
G10 tests retain 1000 complete operation samples, callback ownership, abandon/stop and wrong-session proofs.
The new texture fixture records complete upload latency and four-draw CPU cadence, both idle and uploading;
cadence includes scheduling and the unchanged 1 ms server sleep, and is NOT GPU frame time.

Optional measurement uses one fixed pair of timestamps per staging page, with real queue timestamp bits,
period and wrap mask. Results are read only after that page's real ticket completes, without WAIT_BIT.
An initial measurement attempted vkCmdResetQueryPool on a transfer-only family; Validation correctly rejected
it. The corrected path explicitly negotiates hostQueryReset and resets only after the preceding page ticket.
Failure log is retained; no upload/SPSC/lifetime algorithm was changed to hide it. Query intervals include
copy and in-command barriers (and clone for Region), exclude CPU prepare, readback, queue scheduling and
the separate final Graphics acquire. BC support is an explicit host option; RGBA-only hosts don't require BC.

Release complete regression reached 72/72, preserving all previous 62 names. Additional Native tests prove
partial replacement failure keeps last-good and its accepted first ticket, and resident-byte rejection occurs
before any submit. Debug, ASan and independent final-I verification are still required for final qualification.

### Host/SDK usage and bounds

Set RenderServerConfig.uploads host user/enqueue/join callbacks (bounded, never inline, joined on shutdown).
Select transfer_queues and optional texture_compression_bc / uploads.measure_gpu explicitly. Register
textureFeatureRegistration() before run. A Client Hello/binds UploadTexture to TexturePublished, then submits
the neutral TextureInfo + ExternalDataRef with existing RenderPayload ownership. Set retained_byte_limit to
cover the complete original vector backing and headers. Completion is GPU-ready only after final ticket
completion and publication. Use returned TextureId/revision for replacement, region or explicit release.
No Scene or View is needed to upload; creation Client does not own the resource.

Defaults: four 1 MiB staging pages, 256 MiB pending source budget, 128 physical descriptor-version slots,
256 MiB resident VMA image bytes. Descriptor capacity is 16..4096, multiple of 16 (prepared StableSlotMap
blocks); versions under old GPU/Context leases consume the same fixed budget. Each upload leases one page;
there is no overflow allocation. A row must fit one configured chunk. Region writes clone the whole image
to preserve in-flight readers, so the resident budget must accommodate both versions. This is an explicit
correctness/performance limitation, not sparse residency. Full replacement with incompatible dimensions,
format, layers or mip count while consumers are leased is rejected. G11A supports only 2D single-layer
RGBA8 UNORM/SRGB and BC1 SRGB, explicit mip bytes, nearest sampling and integer explicit LOD.
No automatic mip generation, Mesh/Material storage, WSI/HDR, G11B–D/G12/G13 work is included.

## Implementation handoff for independent V

Internal full matrices: RelWithDebInfo 72/72 (135.16 s), Debug 72/72 (138.24 s), MSVC ASan 72/72
(179.08 s), all builds successful and second builds no-work. Name-set comparison against the previous
62-test exact-I logs has no removal/rename and exactly ten additional top-level cases. Final bounded
review made TextureVersion explicitly immovable, added compressed-region/premultiplied-input negatives,
and recorded fixed-buffer four-view cadence via the existing test-only Native seam. After those final
changes, all three complete builds/second builds and nine affected Texture/SDK tests passed again.
GPU Validation/SyncValidation and ASan report zero errors in these successful runs; overlay warnings remain.
Independent final-I V must still reconfigure/build/run ALL 72 on the exact clean commit, never rely on this
implementation workspace or the targeted reruns alone. Old failures remain in the external evidence directory.

Measured Native host metadata: Binding32, ExecutionContext224, RetirementEntry32, 10k allocation/binding
records1,343,640 bytes/61 new calls (cold ledger benchmark). IpcMessage144/IpcReply296/RenderRequest216 and
Permit16 are unchanged. DeferredCompletion512 (previous448) remains a fixed configured vector, default128
slots; increase8KiB per default Server. No unbounded preallocation or hidden request table was introduced.
