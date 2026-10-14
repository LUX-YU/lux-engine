# G5-C → G9 batch working note

Baseline: `9dc4fcf1146e375172bb970d3a335b70e1145637` (G5 V).
Draft 7.3 and Batch Plan v7 are the inputs. All five handoff files match their SHA-256 manifest.
The primary checkout is deliberately untouched; implementation uses the attached `render-g5` worktree.
This is a working note, not the final independent V or a replacement for any historical qualification.

## Completed internal gates

- M0: explicit dependency authorship intervals; both edge endpoints must belong to the contributing Feature.
  Deduplicated no-op edges remain no-ops. G4/G5 tests, installed consumers and all Render regressions: 35/35.
- M1: real Vulkan InstanceContext, DeviceSession, unique aliased queue owners, VMA resources and retirement.
  Foundation and fault tests execute on NVIDIA GeForce RTX 4070 Laptop GPU. Native Validation and
  SyncValidation errors: 0. The loader reports an Epic Overlay duplicate-layer warning, retained in logs.
  Native ASan tests: 2/2. No device/queue/allocator is linked from Legacy.
- M2: the exact G5 Analysis feeds Native recipes, real Shader/Pipeline and ResourcePlan. Transfer and
  graphics fixtures compile; unsupported Target/Shader/stage/capabilities are rejected. Fault injection
  verifies rollback; Buffer/Image creation counters during compile are both 0. Relocated Native SDK
  consumer creates and destroys the real compiled pipeline. This does not yet execute a G9 Graph.
  Full affected ASan regression including relocated consumers: 39/39. Non-ASan Render regression: 38/38
  before adding the new Native installed consumer; the subsequent four Native tests including it pass.

Evidence: `E:/RenderV2Rebuild/G5C-G9/evidence/`, command runner `../run.ps1`, build trees `../build/`.
All commands capture native stdout/stderr. Build command is `cmake --build <build> --target all -j 4 -- -k 0`;
second builds report no work. Tests use `ctest --test-dir <build> --output-on-failure -j 1 -R '^render\.'`.
Final qualification still requires the exact final I in an independent clean clone.

## G8 bounded input-owner API extension (authorized)

The integrated command §1.1 requires complete reuse of IPC R2 API, while §6.3 requires zero-copy retention
of an existing Asset owner, whole retained-owner byte accounting and real request correlation.

Current source evidence:

- `modules/platform/ipc/include/lux/engine/platform/ipc/IpcMessage.hpp`: `takeOwner(owner, bytes)` stores
  one private erased owner and one contiguous byte view. The only public data accessor is `bytes()`.
- `ReceivedRequest::message()` returns that message; it exposes no typed owner or attachment views.
- `modules/platform/ipc/test/Ipc.cpp`, the existing protocol fixture, retains an external `weak_source`
  and explicitly labels its `weak_source.lock()` as **test-only upper-layer access**. It tests correct
  byte pinning, but is not a production transfer path for separate encoded headers and asset attachments.

Minimal counterexample: two owners hold identical encoded request headers H, and different separately
allocated asset buffers A and B. Passing `takeOwner(owner, H)` delivers indistinguishable bytes H;
the receiver cannot obtain A or B through any R2 accessor. Passing A instead loses H. Joining them
requires a deep copy, putting a pointer into wire, or a separate shared table. The latter two violate
the batch's wire and single-IPC constraints; the first violates existing-owner zero-copy requirements.
Separately, a small SharedBytes subspan can pin a much larger owner. `bytes().size()` is not its cost.

Executable reproducer: `E:/RenderV2Rebuild/G5C-G9/ipc-boundary-proof/Main.cpp`, built against the freshly
installed R2 shared library. Its two distinct 1 MiB assets remain alive while the receiver sees identical
16-byte headers; both owners release once at cold teardown. Raw output is
`E:/RenderV2Rebuild/G5C-G9/evidence/G8-ipc-boundary-proof.log`.

The integrated command §2 explicitly permits demonstrated minimal Core/Data/Protocol/IPC changes.
The user additionally authorized exactly this extension: at most four borrowed attachment views in
the existing IpcMessage, owned by its existing unique holder. SPSC, Channel/Link, publication, reply
credits and reclamation algorithms remain unchanged. G2 Wire.hpp is unchanged. RenderPayload derives
retained cost from the complete vector capacity, never visible subranges. A received request pins
the G2 adapter's SharedBytes; every borrower ends before reply moves input ownership back.
Do not restore a Connection, add a side-channel registry, or rewrite historical IPC PARTIAL.

M3 adds real RenderClient/RenderServer, G2 field codecs, complete schema capability binding,
connection-local RequestId SlotMaps, Server Operation/Scene/Target/Effect SlotMaps and Scene-owned Views.
One Native session initializes in run() before Ready. Effects own compiled code. No G9 attach is
claimed at this checkpoint. At most Main + Upload are admitted per epoch; clients are long-lived.
The tests cover equal A/B RequestIds, a saturated paused A while B/ticks progress for 1.5 seconds,
stale View generation, changed full schema rejection, failed device initialization, whole-backing
budget saturation and recovery, retained replies, abandon collection, controlled close during a
Feature's actual attachment use, exact destruction and new server epoch identity rejection.
Release/ASan regressions: 52/52 before adding Runtime installed consumers; affected tests and new
relocated client/server consumers are rerun after the final API changes. Native Validation + SyncValidation
remain enabled, with zero errors. M4/G9 and final independent V are still pending. G10–G13 not started.

IPC ABI comparison: IpcMessage 160→176 B, IpcReply 328→360 B on Windows x64; inline capacity remains
128 B. The additional two words borrow the stable owner descriptor array. This requires rebuilding
DLL consumers; binary ABI compatibility is NOT claimed. Existing no-attachment behavior tests and
relocated EXE/DLL consumers pass. Raw current cost samples are retained; historical PARTIAL unchanged.

Root TOOLCHAIN was reconfigured from current sources and remains BLOCKED at
`modules/function/CMakeLists.txt:30–32`: missing `render/client`,
`render/features/cmake/feature_client.cmake`, and `render/standard_content`, followed by their missing
install targets. No placeholder targets or Legacy linkage were added. Linux/Android/TSan are NOT_RUN;
the later complete frame hot-path allocation and GPU timing measurements are not yet applicable to
this compiler-only checkpoint. Historical IPC R2 performance PARTIAL remains unchanged.

## M4 implementation gate

G9 now prepares and executes the real compiled Graph through Runtime attach/readback. An Effect owns
the sole vector of four-field ExecutionGroups. DeviceSession owns stable contexts and scoped backing;
contexts pin immutable code and retain scoped leases through actual ticket retirement. Two Scenes,
four Views share one code/pipeline, with Global/Scene sharing and View/Attachment isolation. Graphics
triangle center/edge colors and transfer clear pixels are read from GPU buffers, not synthesized.
Compatible resize preserves TargetId and advances target_epoch; unsupported sample changes preserve
last-good. Detach/remove retain accepted work, and a failed later submit cannot erase its earlier ticket.

Readback completion checks actual GPU tickets. Controlled completion-observation tests prove explicit
Stop produces Cancelled and requester closure keeps backing until the real final fence wait. All
ReceivedRequests are settled before Responder reset; Native resources drain before Instance destruction.
Main paused with 64 unconsumed requests for 1.5 seconds does not stop Upload or actual GPU submissions.

Internal final source matrix: Windows RelWithDebInfo, Debug and ASan each 56/56 tests, all-target build
and second no-work build. Validation + SyncValidation error count 0; loader overlay warnings retained.
Stable frame window has 0 Engine C++ new/aligned-new calls, no new compiler/Feature/scope work. Native
record/submit raw samples and full Client encode/send/handler/reply/decode samples are separate; the
latter allocates and includes the current sleeping host loop. GPU timestamps are NOT_MEASURED.

Three alternating old/current IPC no-attachment benchmark runs retain all raw CSVs under
`evidence/ipc-comparison`. Old baseline is exact M2 f4ab9433, independently cloned/built. 128 B roundtrip
p99 ranges 700–1000 ns old and 600–2300 ns new in these runs; this is variable observational evidence,
not a performance equivalence or tail-resolution claim. Full small Operation same-thread p50 is
200–300 ns current versus 300 ns old; tested EXE/DLL allocation counts remain 0. ABI growth is recorded
above; at depth 64 the two rings' element payload increases by 3072 B, excluding queue/control overhead.

No frozen IPC publication/credit/reclamation code changed: the new checker compares both algorithm
sections of Ipc.cpp byte-for-byte after line-ending normalization. All 719 Legacy blobs, non-whitelisted
tracked files and both primary checkout user-file hashes pass protection checks. M4 still requires
the final independent clone qualification; no final batch PASS is claimed by this implementation note.
The root closure remains blocked at obsolete root render/client wiring and unimplemented Feature Client
and Standard Content consumers, despite the now-real standalone render_client. G10–G13 NOT_STARTED.
