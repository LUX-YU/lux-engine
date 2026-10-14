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

## G8 input-owner API issue awaiting a narrow scope decision

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

Proposed bounded change, NOT implemented without authorization: add read-only attachment views to the
existing IpcMessage, owned by its existing unique holder; preserve SPSC, publication and reclamation
algorithms. The Render-side typed payload factory must derive retained cost from the complete backing
owner (e.g. vector capacity), not visible subranges. Extend move/failure/close/deferred-owner tests.
Do not restore a Connection, add a side-channel registry, or rewrite historical IPC PARTIAL.

M3/G8 and M4/G9 have not passed. G10–G13 are not started. The final integrated V does not exist yet.

Root TOOLCHAIN was reconfigured from current sources and remains BLOCKED at
`modules/function/CMakeLists.txt:30–32`: missing `render/client`,
`render/features/cmake/feature_client.cmake`, and `render/standard_content`, followed by their missing
install targets. No placeholder targets or Legacy linkage were added. Linux/Android/TSan are NOT_RUN;
the later complete frame hot-path allocation and GPU timing measurements are not yet applicable to
this compiler-only checkpoint. Historical IPC R2 performance PARTIAL remains unchanged.
