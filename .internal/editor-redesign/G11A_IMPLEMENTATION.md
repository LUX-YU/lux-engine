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

G11A-3..5 and independent V have not yet run. G11A overall is NOT_COMPLETE; no PASS or push authorized
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
