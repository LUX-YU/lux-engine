# Draft6 G3-R independent verification

**G3-R: PASS. Full product TOOLCHAIN closure: BLOCKED. G4: NOT STARTED.**

Date: 2026-10-13. This qualification covers pure Protocol and internal 1:N in-process connections.
It does not qualify a business RenderClient/RenderServer, Feature installation, GPU work or Engine integration.

## Identity and evidence

| Item | Exact identity |
|---|---|
| Branch | `codex/render-v2-rebuild` |
| Implementation I | `1cd518fd8e18063f8a36ded29a19fd8bfc3bbbda` |
| I parent / original local and remote HEAD | `411c5dcce06a7d259aa6783b71d3a14e63f96cec` |
| V parent | Exact Implementation I above |
| Frozen Legacy | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| lux-cxx | `bf779515a120350c7c5412c366eea73afdf59ccf` |
| lux-cmake-toolset | `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60` |
| Draft6 SHA256 | `c938b9e670049acaeebd3218f0a6197525caca4069051877711b6df316b88ba9` |
| Plan v3 SHA256 | `16172b2203db1c61b1cba36b946c0750ae2cc133568c7cc9ff113073495d367d` |
| Command v2 SHA256 | `479260c9903f98f1aefb7006958af34d2d9bf71688f13e4983a60e7bbb73da98` |
| Consistency audit SHA256 | `a4c2874ced91b868aed0caf3a908cbcb040a5cc2b54b5ebfe9733fb165a4aa7b` |

V is the report-only commit containing this file. Its full SHA is supplied in the delivery message
and external delivery.json; placing a commit's own hash inside itself would be self-referential.
No push or merge is included in this stage.

Independent source: `E:/RenderV2Rebuild/G3R/verify/source`, cloned with `--no-hardlinks`
and detached at exact I. The frozen Legacy commit was fetched by SHA from the original repository.
Git emitted its shallow-root warning; the needed commit/tree/blob objects were readable, and
the frozen/HEAD/worktree comparisons all passed. No verification source was repaired or modified.
ValidateTrackedSnapshot ran **before configuration**. All **2,982 tracked files** were SHA256
snapshotted before and after qualification and stayed byte-identical; Git status stayed clean.

Final evidence: `E:/RenderV2Rebuild/G3R/verify/evidence` contains command output, LastTest logs,
tracked snapshots, protection reports, actual CMake target DAGs, all nine installed-consumer
header checks, and raw tick/queue/roundtrip CSVs. Implementation investigation and installation
backups remain separately under `E:/RenderV2Rebuild/G3R/evidence`. Earlier failed experiments
are retained there and are not substituted for final-I qualification.

## Actual gates

| Gate | Independent exact-I result |
|---|---|
| RelWithDebInfo all targets | PASS, 14 translation units, 21 Ninja actions |
| Debug all targets | PASS, same C++20 `/GR-` closure |
| ASan all targets | PASS, `/fsanitize=address /Zi`, `alloc_dealloc_mismatch=1` |
| Second all builds | PASS in all three matrices, `ninja: no work to do` |
| RelWithDebInfo CTest | PASS, 20/20, 10.28 seconds |
| Debug CTest | PASS, 20/20, 10.45 seconds |
| ASan CTest | PASS, 20/20, 11.66 seconds; no sanitizer diagnostic |
| G1/G2 regressions | PASS, all original 14 tests preserved and rerun |
| Connection qualification | PASS, contracts / ownership / races / progress / cost |
| Installed consumers | PASS, Core/Data/Protocol in each of three matrices, nine fresh relocated SDKs |
| Public header closure | PASS, six Render headers independently compiled; exactly nine canonical installed headers per SDK |
| Protocol target | PASS, public dependency exactly `render_data`; transitive Core/lux-cxx only |
| Internal Connection | PASS, non-exported library, project-only header; no SDK Connection or Transport target/header |
| Source/compiler dependency audit | PASS, no Vulkan, Legacy, Engine/Editor, ECS or future Native/Client/Server dependency |
| Protected files | PASS, 719 Legacy blobs, guard, Core/Data, historical reports and user settings unchanged |
| Root TOOLCHAIN | BLOCKED, known missing future consumers, detailed below |
| GPU / Vulkan Validation / SyncValidation | N/A |
| Linux / Android builds / TSan | NOT_RUN; ASan is not a data-race detector |

Actual CMake file-API DAG: Protocol -> Data -> Core; Connection -> Protocol/Data/Core with
private lux-cxx concurrent/container dependencies. Installed Protocol has no dependency on Connection.
Unchanged historical bootstrap scripts still mention Transport in their old qualification paths;
they are not configured by this build and were not rewritten to alter historical qualifications.

## Concurrency, identity and lifetime evidence

- Two threads concurrently connect through the same address. Before bounded server accept neither
  endpoint is Ready; afterwards both bind the same immutable server OperationId. Missing/duplicate
  addresses, close-before-accept, stop-before-accept and reused-address old-server bindings are tested.
- Both clients intentionally allocate **equal numeric RequestIds**. Deferred work completes in reverse
  order and each result returns to its originating ring. Each endpoint has one request producer/consumer
  and one reply producer/consumer. There is no central mutable RequestCell or global business queue.
- The only admission CAS list is cold Connect handoff. Nodes own backing before publication; the server
  owns reclamation after exchange. Connect callers must finish before unlisten/destruction. That
  unsupported overlap is explicit, rather than being presented as safe concurrent destruction.
- Request-ring fullness, reserved Reply-credit exhaustion, byte-budget exhaustion and attachment-count
  rejection have negative tests. A failed raw Packet submission preserves the original pointer and data.
  Abandon does not return credit until its terminal is drained; generations reject the old local ID.
- A real 1 MiB owner retains the exact data pointer through queuing and deferred use. Destruction counters
  prove one release after the final use; abandon and disconnect do not release active input early.
  Cold collection refuses an active deferred endpoint, then reclaims only the closed connection.
- A move-only ServerWork is the sole deferred completion owner. Wrong reply schema becomes Failed;
  void success is terminal; dropped work becomes Cancelled; duplicate completion is rejected.
  Thirty-two barrier-controlled runs cover close/consume before completion, published terminal before
  Stop, and Stop before completion. Unknown execution is reported as OutcomeUnknown, never fabricated
  success or known cancellation. Late completion cannot publish a second observable terminal.
- The server-global fixture token remains usable by A after uploader B disconnects. This proves the
  server/connection lifetime distinction only; no GPU storage or cross-server resource-key capability
  mechanism is claimed. Bare resource SlotKeys require server-identity partitioning by future consumers.
- Ordinary input release moves to Client consumption with the Reply. Disconnected backing collection and
  shutdown are explicitly cold maintenance after outstanding uses finish; arbitrary large-owner destructors
  are not allowed inside the bounded receive loop. No hard real-time destruction guarantee is claimed.

The production lock inventory is empty: no introduced mutex/shared_mutex/condition_variable,
spinlock/atomic_flag, atomic<shared_ptr>, MPSC/MPMC business queue or shared Pending map.
Pointer, queue-index and flag atomics are statically required to be lock-free. The existing lux-cxx
`SpscLockFreeRingQueue` supplies the actual release/acquire queue algorithm. Thread affinity violations
terminate in all configurations; moving ownership requires an externally synchronized handoff.

## Independent progress and actual operation cost

RelWithDebInfo final run: during A's **1.2 second pause**, B completed **1,187,668** owned-upload
replies while the independent server CPU tick advanced **670,786** times. A's eight unconsumed
credits did not stop B. With B continuously attempting excess work, A then completed all 200
requests; its FIFO values remained correct. Receive scheduling uses a total budget of eight,
a per-connection budget of two, a rotating cursor, and a separate bounded accept budget.

Tick samples span the run at a stride of 64: **p50 1.8 us / p95 2.3 us / p99 2.8 us**;
sample max 109.3 us, maximum over **all** ticks 314.2 us. The run saved 10,492 tick samples
and 3,546 queue/byte samples. Maximum B request depth was eight; admitted input bytes peaked at
416 (eight 20-byte records plus eight 32-byte visible attachments). These are CPU communication
and scheduling observations, not Vulkan frame performance or a real-time deadline guarantee.

The allocation fixture measures the entire small-operation path: G2 codec -> submit -> SPSC
receive -> handler decode -> reply encode -> Reply SPSC -> client consumption/decode. It is
a same-thread round trip through both endpoints, so it excludes cross-thread scheduling latency.
Payloads are encoded on every iteration; no preencoded queue microbenchmark is substituted.

| Configuration | Cold assembly allocations / bytes | First full operation allocations / latency | 10,000 warm full operations allocations / bytes | Warm p50 / p95 / p99 / max (us) |
|---|---|---|---|---|
| RelWithDebInfo | 108 / 17,472 | 0 / 13.3 us | **0 / 0** | 0.5 / 0.5 / 0.8 / 16.0 |
| Debug | 159 / 18,680 | 0 / 29.3 us | **0 / 0** | 8.4 / 9.9 / 14.1 / 255.6 |
| ASan | 108 / 17,472 | 0 / 55.4 us | **0 / 0** | 10.8 / 15.9 / 21.0 / 299.3 |

Cold assembly includes two connections and binding. Source bytes use existing SharedBytes;
attachment lists with actual entries still allocate/copy owner references, not the resource bytes.
The admission byte budget charges retained request wire and visible attachments until terminal
consumption; reply storage is separately bounded by credits, reusable block count and per-message
wire limit. This is not a global allocator accounting system. Retained output can occupy a reusable
block and cause a structured capacity failure. No G2 codec or source ownership contract changed.

## Reproducible commands and protection

The external runner first loads VS 2022 `Launch-VsDevShell.ps1 -Arch amd64 -HostArch amd64`.
It uses CMake 3.31.10, Ninja and the source-matched isolated lux-cxx SDK (64 headers).
Representative exact configuration, repeated for Debug and ASan:

```powershell
$s = 'E:/RenderV2Rebuild/G3R/verify/source'
$b = 'E:/RenderV2Rebuild/G3R/verify/build'
cmake "-DLUX_SOURCE_DIR=$s" -P "$s/cmake/ValidateTrackedSnapshot.cmake"
cmake -S "$s/cmake/render-v2-bootstrap" -B $b -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON `
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
  '-DCMAKE_CXX_FLAGS=/EHsc' -DCMAKE_CXX_SCAN_FOR_MODULES=OFF `
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF `
  "-DCMAKE_INSTALL_PREFIX=$b/sdk"
cmake --build $b --target all -j 4 -- -k 0
cmake --build $b --target all -j 4 -- -k 0
ctest --test-dir $b --output-on-failure -j 1
```

Debug uses `-DCMAKE_BUILD_TYPE=Debug`. ASan uses RelWithDebInfo and
`-DCMAKE_CXX_FLAGS=/EHsc /fsanitize=address /Zi`; `ASAN_OPTIONS=alloc_dealloc_mismatch=1`.
CTest runs fresh install -> prefix rename -> external find_package -> all build -> consumer test.
`check_g3r.py --root <clone> --clean --build <build>` checks stage/dependency/protection boundaries;
`--installed <each relocated prefix>` checks exact public header sets and bytes. The orchestrator
`E:/RenderV2Rebuild/G3R/qualify.py` records every command and fails on an unexpected exit code.

The Legacy manifest still has exactly 719 entries, all matching frozen Git blobs, I and worktree.
Every tracked modification outside the allowed G3-R paths is rejected. Core/Data, Legacy CMake
guard, G0–G3 reports and old checker scripts are unchanged. Original settings content and binary
patch match the G1 protection snapshot; settings SHA256 remains
`f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`.

The Implementation diff is **33 files**: 14 additions, 18 old Transport deletions, one Render CMake
entry modification. Production additions are Protocol.hpp/Protocol.cpp and Connection.hpp/Connection.cpp;
the remainder is module/test/installed-consumer CMake, two test sources, implementation notes and
the new checker. Full name/status inventory: `verify/evidence/implementation-files.txt`.
V adds only this report. No verification source or implementation fix is mixed into V.

Three shared include prefixes were checked: Debug, RelWithDebInfo and Android/lux-engine. New Protocol
is synchronized, and all six current Render headers match original source bytes. The twelve old
Transport headers were backed up and removed only after matching their baseline contents. Existing
Data installed headers differed solely by CRLF/LF; their six copies were backed up and synchronized
to source bytes, without any Core/Data source change. All originals and SHA256 values are recorded
in `evidence/header-sync.json`; Android include synchronization is not Android build qualification.

## Remaining limits and stop

Root TOOLCHAIN was reconfigured in the independent source and still fails for absent
`render/client`, `render/features/cmake/feature_client.cmake`, `render/standard_content`,
`render_client`, `render_feature_client`, and `render_standard_content`. This expected product
closure remains **BLOCKED**, independently of G3-R's passing module qualification. No fake target,
consumer deletion, disabled test or historical qualification change was introduced.

Earlier implementation-only failures (timer granularity in the first fixture and Debug STL empty
attachment proxy allocation) were fixed before I; their logs remain available. Exact-I independent
qualification had no failed mandatory gate. TSan and other platform builds were not performed.
One published listener address and externally ordered connect/unlisten lifetime are the current
supported scope. Full Feature capability installation, Engine/stdexec adapters, GPU execution and
unsolicited events remain outside G3-R.

**STOP. Next stage remains G4 and requires user review/authorization.**
