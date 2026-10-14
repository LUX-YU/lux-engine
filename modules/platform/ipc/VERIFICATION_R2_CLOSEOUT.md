# IPC-R2 bounded closeout — independent verification

Overall **PARTIAL**: the historical 128 B single-thread slow interval remains
unexplained. Lifecycle, targeted concurrency, build, ASan and installation checks
pass. This is a bounded closeout, not a new architecture stage. No G4/G8 work or
speculative performance optimization was performed. Historical qualifications
remain unchanged. Complete product closure is separately **BLOCKED**.

## Identities and boundary

| Item | Identity |
|---|---|
| Branch | `codex/render-v2-rebuild` |
| Baseline, I parent, remote HEAD at start of verification | `b18acab626370d9c8c8bd4598009cce533c660d1` |
| Implementation I / V parent | `59953404510baca1480f4d8ee2125f8cc2c50165` |
| lux-cxx (clean) | `bf779515a120350c7c5412c366eea73afdf59ccf` |
| lux-cmake-toolset (clean) | `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60` |

V only adds this report. Its full SHA is recorded in the delivery response and
external delivery.json; `git log -1 --format=%H -- modules/platform/ipc/VERIFICATION_R2_CLOSEOUT.md`
also identifies it without a self-referential commit hash.

I changes exactly five files under `modules/platform/ipc/`: README.md,
pinclude/TestHooks.hpp, src/Ipc.cpp, test/CMakeLists.txt and test/Ipc.cpp.
The only production behavior change checks `output.active()` once on entry to an
active responder, before scanning Links. Invalid active output is rejected even
when no Link is ready. Two publication hooks compile only with LUX_IPC_TESTING.
SPSC algorithms, message movement, memory ordering and public headers are unchanged.

Independent no-hardlinks clone: `E:/RenderV2Rebuild/IPC-R2-Closeout/verify-source`,
detached at exact I. ValidateTrackedSnapshot passed before configuration. The clone
remains clean; 2,998 tracked file SHA256 values match the recorded audit snapshot.
No production or test repair was made in this clone.

Evidence, raw CSVs, diagnostic source, commands, logs, manifests and checksums:
`E:/RenderV2Rebuild/IPC-R2-Closeout/qualification/59953404510baca1480f4d8ee2125f8cc2c50165/`.

## Tail investigation — cause unresolved

The existing pinned run `IPC-R2/verify/evidence/pinned/0-after/ipc-roundtrip.csv`
has SHA256 `e9f28482bd1b32cab0c471e69b58fb640aaf48f6542315d71a5e788c54bcc47a`.
Its 128 B samples show a contiguous interval, not merely one extreme outlier:

| Iteration interval | 500-sample window p50 / p95, ns |
|---|---:|
| 0–3999 | p50 1100–1200; p95 1200–1300 |
| 4500–4999 | 1800 / 1900 |
| 5500–5999 | 2100 / 2400 |
| 6000–6999 | 2300 / 2500 in both windows |
| 8000–9999 | p50 1400–1500; p95 1500 |

Across the original three pinned after-runs, 128 B p99 was 2500, 1400 and 1300 ns.
The original three non-pinned pairs did not show the same persistent tail regression.
This establishes the observed shape and its inconsistent reproduction, not its
cause. Historical samples contain no simultaneous thread-cycle, context-switch
or CPU-frequency trace. They cannot distinguish scheduling, code execution cost
or another system effect. None of those possibilities is reported as a finding.

A bounded supplementary diagnostic uses the exact previous R2 production DLL
(BUILD_TESTING=OFF), fixed logical CPU mask 1, the same 128 B scalar-plus-zero-array
field format, and full input validation. It alternates codec-only, preencoded IPC
and full codec/IPC phases. Each of three runs retains 1,000 timed batches per phase,
256 operations per batch, after 20 warmup rounds. File output happens after timing.
Wall time and QueryThreadCycleTime are recorded together; source/binary hashes and
all raw values are archived. No build or other benchmark ran concurrently.

Batch-average ns/operation, p50 / p95 / p99 (not individual-request percentiles):

| Run | Codec only | Preencoded IPC | Full codec + IPC |
|---|---:|---:|---:|
| 0 | 390 / 441 / 689 | 125 / 134 / 160 | 622 / 729 / 850 |
| 1 | 401 / 438 / 661 | 129 / 141 / 161 | 664 / 785 / 895 |
| 2 | 400 / 483 / 675 | 129 / 140 / 188 | 650 / 799 / 1311 |

These batches still include outliers (full-path maximum batch average 15,520 ns/op).
No consecutive full-path batches exceed 1,800 ns/op. This descriptive threshold
is not a PASS criterion. Batching, phase interleaving, production instrumentation
and call-site code generation differ from the historical harness; these results
cannot establish a speedup or explain away that harness's slow interval. Thread
cycles are retained as an independent observation, not converted into elapsed time
or inferred CPU frequency; see Microsoft's [QueryThreadCycleTime contract](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime).
The cause remains **UNRESOLVED**, so the performance qualification stays **PARTIAL**.
No further benchmark campaign or production tuning is authorized by this closeout.

## Frozen lifecycle and memory bound

README now specifies the future Render integration contract: one Main and at most
one Upload Requester, **at most two cumulative creations per Server epoch**. These
are long-lived participants, not task/asset/frame objects. Startup retries end the
old epoch first. High-frequency reconnect/churn is not permitted by this contract;
the generic IPC factory does not enforce a Render-specific participant count.
A future high-churn consumer must first qualify cold closed-Link reclamation.

Default 64-credit Links bound accepted requests with unconsumed replies to 128
total. Request queues, borrowed work and reply queues share those credits. Windows
x64 Ring element storage is 62,464 B for two Links, excluding metadata and external
allocations. Closing one Requester does not release its Link or queued payload
while another role retains the epoch.

Payload bytes remain **unbounded without an upper-layer budget**. A holder can
retain more backing than its visible span. G8 must provide finite retained-owner
budgets B_main + B_upload, covering input, results and application-held replies
until final release, including after reply credit return. This is a concrete G8
entry requirement, not an implemented budget or a current memory-bound claim.

Future Server order is frozen as stop admission → cancel and join borrowers →
settle/reset every ReceivedRequest on the responder owner → release Responder →
release remaining Requesters in a cold context. Partial initialization unwinds
in the same order; cancellation request is not join completion. CPU settlement
does not establish future GPU completion. G8 must test its actual implementation;
this stage only tests the recipe, without creating a RenderServer type.

## Targeted concurrency and teardown tests

The new `platform.ipc.closeout` group retains the existing 90-second failure timeout:

- 32 repetitions paused after observing open but before reply Ring publication,
  and 32 paused after publication but before clearing the request's source borrow.
  Requester reset completes while paused. Input/result allocations remain at the
  same addresses and are released exactly once at final Responder teardown.
- Close observed before publication returns CLOSED, preserving both input and
  result owners and the active source. Explicit disposal releases each once.
- Normal completion, partial initialization failure and background cancellation
  use the required join/settle/reset order. Worker bytes remain readable through
  cancellation; live-peer failure emits ABORTED, and published replies remain
  consumable after responder close and can outlive both roles.

An already-started reply may succeed after requester close. That success proves
safe publication, not consumption by the closed peer. It creates no new business
success/cancellation guarantee. All responder operations/request disposal remain
serialized; workers only borrow bytes and are joined before disposal.

## Commands and results

Windows x64, MSVC 19.44.35226, CMake 3.31.10, Ninja, i9-13900H.
Archived run.ps1 and production.ps1 supply the VS environment and exact paths:

```text
cmake -DLUX_SOURCE_DIR=<clone> -P <clone>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <clone>/cmake/render-v2-bootstrap -B <build> -G Ninja
  -DCMAKE_BUILD_TYPE=<RelWithDebInfo|Debug> -DBUILD_TESTING=ON
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx
  -DCMAKE_CXX_FLAGS="/EHsc [ASan: /fsanitize=address /Zi]"
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1
py -3 <clone>/modules/platform/ipc/test/check_r2.py --root <clone> --clean
```

| Configuration / gate | Result |
|---|---|
| Independent RelWithDebInfo | 25/25 CTest; full all build; second build no work |
| Independent Debug | 25/25 CTest; full all build; second build no work |
| Independent RelWithDebInfo + ASan | 25/25 CTest; no ASan diagnostics; second build no work |
| BUILD_TESTING=OFF | Full all build; second build no work; fresh relocated IPC and IPC+Protocol consumers 2/2 |
| Existing Core/Data/Protocol, headers and installed checks | Preserved and passed |
| Warm small-message allocation checks | EXE and DLL counts zero at 8/32/64/128 B; generic G2 encodeWire still two allocations |
| Linux / TSan | NOT_RUN: local WSL is not installed; Windows ASan is not a race detector |
| Android configure/build, GPU/Validation/SyncValidation | NOT_RUN for Android; N/A for GPU checks |
| Root TOOLCHAIN configure | BLOCKED: missing render/client, feature_client.cmake, standard_content and their export targets |

Actual compile commands prove C++20, /GR-, and ASan on all affected test/DLL TUs
in the ASan configuration; ASAN_OPTIONS=alloc_dealloc_mismatch=1. Production OFF
commands exclude LUX_IPC_TESTING. Source/include/link/install audits find no
IPC production dependency on Render, Legacy, Vulkan, ECS or Editor. Three test
builds each have 20 TUs; production has five. All four fresh install sets contain
the expected 12 headers with exact bytes. No empty target or disabled test was added.

The independent progress run records B completing 2,890,769 requests and mock Tick
advancing 7,203,214 times during A's 1.5-second pause; this is a test service, not a
rendering guarantee. Existing FIFO/source isolation/grace tests remain active.

Protection: frozen Legacy 719 blobs match commit and worktree; all files outside
the five-file I whitelist remain unchanged from baseline, including historical
reports/checkers and Core/Data/Protocol contracts. User settings SHA256 remains
`f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`.
No public header changed; the existing Debug/RelWithDebInfo/Android prefix copies
(nine IPC header/visibility entries) were checked unchanged rather than rewritten.

Audit harness corrections: the new clone initially lacked the disconnected frozen
Legacy object, which was fetched explicitly before its blob comparison; Git emitted
its shallow-root warning but the object comparison subsequently passed. The external
SHA audit initially misread Git-quoted non-ASCII paths; using core.quotepath=false
fixed that harness. Snapshot validation itself passed before configure; file SHA
capture was completed during the first matrix run and remained equal afterward.
Neither issue required a source change. The root configure failure is retained,
not waived. No test timeout or sanitizer failure occurred.

Future IPC cross-platform qualification must run the closeout/grace/directory/FIFO
tests under Linux TSan with timeouts and no blanket race suppressions. That work
is not performed or automatically scheduled here. Remaining entry issues are the
unexplained historical tail and G8's concrete retained-byte budget and host teardown
implementation. Stop after this report and the authorized branch push.
