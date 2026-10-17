# IPC / Runtime convergence — final independent verification

Implementation I: `2bd86e7eb026c44e93ca70043cbcf624dd72bbd2`.
Parent: `ed2f80a1cec1bec14b9437e3505e0b1dc5da6ec9`.
Baseline: `9184c9f71c63af7f686822c76b89263b29ef7a63`.
Contract SHA-256: `5b52a293293b75418598ff3c7314cc7160b3014c08c434d4757328a5fd9f9ef7`.
The Verification commit adds only this report and its evidence index; its own SHA is given after commit.

## Verdict and scope

**Overall: PARTIAL. Render/IPC structural and exercised functional contracts: PASS.**
**Full Debug matrix: PARTIAL (two baseline failures). Performance: PARTIAL. Product closure: BLOCKED.**

The latest user direction was to stop expanding investigation of three preexisting Debug failures and
continue the authorized convergence. Their source and historical failure records were left unchanged.
They are not declared passing or silently removed. Upper Engine/Editor product tests are outside scope.
Performance remains PARTIAL: neither smaller structures nor passing GPU oracles establish equivalence.
No subsequent RenderGraph/material/WSI or Engine integration stage was started.

## Exact-source verification

Implementation worktree: `E:/RenderV2Rebuild/IPCConvergence/source`.
Independent source: `E:/RenderV2Rebuild/IPCConvergence/final2-source`, detached exact I, clean tracked
snapshot, cloned with `--no-hardlinks`, without object alternates. Fresh independent build trees:
`E:/RenderV2Rebuild/IPCConvergence/final2-build/{r,d,asan}`.

The preceding independent attempt on `ed2f80a1c` failed to compile the newly narrowed neutral Server
header probe because its target omitted the neutral `render_protocol_handlers` dependency. No tests were
claimed for that attempt. The dependency was corrected in the implementation tree and committed as final I;
the failed clone stayed untouched. The current independent clone and builds were then created afresh.
Both attempts and their exit codes are archived.

Commands are preserved verbatim in the runner and command logs. Each configuration executed serially:

```text
cmake -DLUX_SOURCE_DIR=<exact-I-clone> -P <clone>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <clone>/cmake/render-v2-bootstrap -B <fresh-build> -G Ninja
  -DCMAKE_BUILD_TYPE=<RelWithDebInfo|Debug> -DBUILD_TESTING=ON
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx
  -Dstdexec_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/stdexec
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_CXX_FLAGS="/EHsc [ASan flags]"
cmake --build <fresh-build> --target all -j 4 -- -k 0
cmake --build <fresh-build> --target all -j 4 -- -k 0
ctest --test-dir <fresh-build> -V --output-on-failure -j 1 -R .
```

ASan uses `/fsanitize=address /Zi`, `ASAN_OPTIONS=alloc_dealloc_mismatch=1`.
Package registry fallback is disabled. Full runner includes remaining package paths and install prefix.

| Configuration | Full all build / second no-work | CTest | Elapsed |
|---|---|---|---:|
| RelWithDebInfo | PASS / PASS | 146/146 PASS | 167.80s |
| Debug | PASS / PASS | 144/146; two baseline FAIL | 281.00s |
| MSVC ASan RelWithDebInfo | PASS / PASS | 146/146 PASS | 264.04s |

Final Debug failures are `object.target_contract` and `graph.edit`. `object.retirement_contract` passed
in this run (10.21s); its earlier baseline/I4 failure remains historical and is not declared fixed.
The failed Object process-death test hits the baseline Debug CRT abort dialog and exceeds its subprocess timeout.
Graph compares an iterator from a subspan with the full span's end; MSVC Debug rejects the incompatible
ranges. The final run's stuck exact Graph process was explicitly terminated after observation and counts
as FAIL, not successful cancellation. No out-of-scope source, test timeout or assertion was weakened.
I4 already reproduced all three on baseline binaries and checked eight relevant source hashes unchanged;
that evidence remains in its immutable archive. No further diagnostic reruns were added in final V.

## Delivered architecture and ownership

- Client production facade, Pending mirror, implicit original-request return, Hello/bind path and generic
  message budgets are deleted. Applications send actual IpcMessage values, own RequestIds, and discover
  actual CommandIds; equal request numbers on different Links do not mix sources.
- IpcMessage owns the real C++ object using private static move/destroy operations. It has no ABI
  negotiation or code pins. Host retains originating modules until values and callback tails end.
- Each existing Link owns its two segmented SPSC queues. Growth failure occurs before moving input.
  Consumer publication controls segment reuse; cold Link destruction frees its unique segment chain.
  Name/Channel retirement, reader grace and source-route lifetime remain the existing system.
- Existing RenderRequest owns one ReceivedRequest and, after execution, the actual result. Failed reply
  publication keeps that result and retries delivery only. Synchronous operations do not allocate async
  slots; real asynchronous work uses the existing growing stable owner and its prepared notification link.
- Command SlotMap owns the hot route; Handler SlotMap uniquely owns stable implementation objects.
  Cold metadata is separate. StorageSet/Registry/DeviceSession and actual GPU tickets remain authoritative.
- RenderServer.hpp is neutral. Selected VulkanServer.hpp provides the factory/typed installation; existing
  Impl owns the sole Instance/DeviceSession and borrows the Host ExecutionRuntime scheduler. No new pool.
- StorageFactory is immutable after typed construction; config equality uses the existing ConfigValue.
  Pins are removed from genuine data/config/async/native work without removing data owners or LastUse.

New production shapes have narrow roles: queue-private Segment/Slot storage; cold CommandMetadata and
hot routing record; selected Vulkan factory; stateless private-access ServerBootstrap. CommandHandler.hpp
moves the existing server-only Reply/thunk contract out of the pure declarations. No new Registry,
Manager, Client wrapper, resource owner or upload state machine was introduced.

Final I relative to baseline: 151 files, +3973/-5767 lines. Production module files excluding examples/tests:
61 files, +1154/-2208. Exact change list and commit parents are in `source-changes.json` in the archive.
Deleted production files include RenderClient.hpp/.cpp, CommandValue.hpp, TypedProtocol.hpp,
MessageAccess.hpp and the pin-only resource Instance.cpp/Texture.cpp. Canonical-ABI-only plugin test is
SUPERSEDED. Valid ownership, source routing, partial-submit, generation, GPU and LastUse tests remain;
old-to-new behavior mapping is in I4_MIGRATION.md and CHECKPOINTS.md.

## Runtime evidence and costs

Final Release four-View oracle: 2 Scenes / 4 Views, 578 measured hot ticks, 2300 submissions,
first-party hot `new=0`, upload progress 295. Real Mesh/Material/Instance, cameras/depth, explicit
readback, LastUse, three Queue modes, Storage extension, stop/close and installed 3D consumers ran.
Windows NVIDIA GeForce RTX4070 Laptop GPU, driver617.42; CPU Intel Core i9-13900H.
Reported GPU Validation/SyncValidation errors0 and ASan diagnostics0. Actual warnings are retained,
including duplicate EOS overlay registration; no claim of warning-free operation is made.
The optional vulkaninfo executable was absent, so no separate API-summary probe is claimed.

Measured Release structures: IpcMessage 128 B / align16 / SBO80, RenderPayload32,
HandlerCall16 / align8, CommandEntry24 / align8, CommandMetadata80, RenderRequest320.
These are layout observations, not performance claims. I3 growth/reuse proof used backlog257,
initial segment size4 and 130 live segments across both directions; 20 reuse cycles allocated no further
segments. This measured backlog is not a promised global memory bound; actual growth consumes memory.

Final Release small Command samples (1,000; ns, nearest-rank):

| Component | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| construct/submit | 100 | 300 | 500 | 5,200 |
| Handler/reply | 200 | 1,000 | 3,200 | 5,900 |
| successful typed consume | 100 | 400 | 1,000 | 168,300 |
| wait and consume | 400 | 2,041,200 | 2,155,200 | 2,541,800 |

Wait/consume includes host scheduling and wait; it is not pure IPC latency. Narrow prewarmed Command
samples report zero measured engine allocations; this excludes real queue growth and asset preparation.
Single-run IPC operation (10,000 each) p95/p99/max: same-thread200/200/4500ns;
two-core600/600/212100ns. Separate 64-byte two-core object samples include a 1,266,200ns maximum.
All raw outliers and per-size summaries are retained; no new repeated sampling was added after final V.

Earlier exact I2/I3 comparison ran 10 alternating independent process pairs with identical direct-message
work: median per-run p95 same-thread200ns both, two-core750ns to650ns. Coarse-clock tails remain
unattributed (including 2.6582ms); this is not a latency improvement qualification. Old implicit-return
workload differs and is not treated as equivalent. Native/Alias remain separately PARTIAL.

Final GPU-ready wall-time traces have 1,000 samples per Queue mode. Default-mode p95
submit-to-observed is15.6057ms and observed-to-publication14.4907ms, with maxima21.1184ms/15.505ms;
dedicated and same-family distributions differ. No cause is claimed without controlled attribution.
Actual CPU observations, publication and consumption are distinct from GPU device timestamps.
GPU timestamp for this comparison NOT_MEASURED; VTune/TSan/Linux/Android build NOT_RUN.
Thread CPU and full retained-memory peak were not newly qualified; performance cannot be overall PASS.

## Installation, protection and evidence

All three configurations build public headers independently and run fresh/relocated consumers, including
pure synchronous Client with Vulkan/stdexec discovery disabled, generic Server without Native include
paths, selected Vulkan Host, external TestBuffer and basic_mesh. Header sync applied81 exact-I paths
(27 per prefix) to Debug/RelWithDebInfo/Android, preserving original-file backups and before/after hashes;
obsolete public headers were removed explicitly. Android header sync is not Android build qualification.

Tracked snapshot and both final checkers passed:719 frozen Legacy blobs; four original user files and
their worktree status/HEAD; dependency identities; historical records; change whitelist. Both implementation
and independent source were clean at final I.

Root TOOLCHAIN configure was actually rerun and failed (exit1) on the existing missing render/client,
render/features/cmake/feature_client.cmake and render/standard_content inputs, with ensuing absent-target
install errors. No placeholder or Legacy fallback was added. Product closure remains BLOCKED.

Dependencies: lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`;
lux-cmake-toolset `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`.
The contract checker scans 124 production source/headers and deleted public surfaces. Historical checkers
and qualification reports are unchanged. Original dirty worktrees are not the implementation or V source.

Archive and SHA-256 are in adjacent EVIDENCE.json. It contains raw commands/return codes, failing attempts,
all configuration logs, per-test logs/CSV, source and dependency identities, SDK backups, manifests and
build/install metadata. Every archived file hash and ZIP CRC was verified. Prior I0 and I4 archives are
referenced by their immutable hashes instead of duplicating them. V does not alter production or tests.

Remote before publication: `9184c9f71c63af7f686822c76b89263b29ef7a63` after final fetch. Push is only fast-forward to `codex/render-v2-rebuild`;
no main merge, force push or user-tree checkout. Final commit/push identity is reported after V creation.
