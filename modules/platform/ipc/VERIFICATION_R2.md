# IPC-R2-FIX independent verification

Overall qualification: **PARTIAL**. Implementation, ownership, concurrency,
ASan and installation gates pass. A single-thread 128 B tail-latency observation
remains unresolved; median improvements do not waive that gate. Root TOOLCHAIN
closure remains separately **BLOCKED**. No G4/G8, push or merge was performed.

## Source identities

| Item | Exact identity |
|---|---|
| Branch | `codex/render-v2-rebuild` |
| Baseline / I parent / actual remote at verification | `d29b6d448b9f7527431ead5fb8765457ce28875d` |
| Implementation I | `0042047e801881e90bab776c29ace0abbb0836d1` |
| V parent | `0042047e801881e90bab776c29ace0abbb0836d1` |
| lux-cxx | `bf779515a120350c7c5412c366eea73afdf59ccf`, clean |
| lux-cmake-toolset | `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`, clean |
| R2 input document SHA256 | `92e81e9be53fbb1370e89a4adb38742a5903099b40e597d6ceaa137b8d85f77b` |

V adds only this report. Obtain V's own SHA using `git log -1 --format=%H --
modules/platform/ipc/VERIFICATION_R2.md`; the delivery response and external
delivery.json record it. A commit cannot embed its own hash in its contents.

Independent source: `E:/RenderV2Rebuild/IPC-R2/verify/source`, a no-hardlinks clone
detached at exact I. ValidateTrackedSnapshot passed before configuration. All
2,997 tracked file SHA256 values match before/after verification; source remains
clean. No implementation repair was made in the verification clone.

External evidence archive:
`E:/RenderV2Rebuild/IPC-R2/qualification/0042047e801881e90bab776c29ace0abbb0836d1/`.
It includes raw samples, commands, failed harness attempts, logs, installation
backups/hashes, complete changed-file list and an evidence hash manifest.

## Actual implementation and lifetime changes

All ten I files are within `modules/platform/ipc/`: CMakeLists, README, both public
headers, private TestHooks, Ipc.cpp, the existing main test, new check_r2.py, and
the two installed consumer files. No Render production contract or historical
report/checker changed. README is the R2 authority for the revised lifetime/API;
historical IPC-D1 documents remain evidence for their original commits.

- Channel owns all Link storage through an immutable ownership chain, separate
  from the non-owning pending/round-robin chain. Link directly contains two lux-cxx
  SPSC queues; the separate internal Rings allocation/type was removed.
- Channel's atomic role_uses counts only role acquisition/release. ReceivedRequest
  borrows Channel/Link and uniquely owns its input; it must settle before Responder
  reset. A responder-only ordinary borrowed_requests counter enforces this in
  release builds too. It does not retain storage and performs no atomic RMW.
- Requester close marks its Link closed and drops the role. Link/Rings remain until
  the final Channel role disappears. Responder close clears its borrowed view and
  closes/detaches the epoch. Only Channel destruction traverses/deletes owned Links.
- Per-message reference increments, Link access CAS, atomic wait/notify and early
  Ring destruction are removed. Existing name lookup/positive role acquisition and
  reader-grace retirement/republication logic are preserved.
- Both receive APIs use EIpcReceive plus output. Only RECEIVED modifies output.
  Reply publication before close is still observable before CLOSED. Failed send
  or reply keeps the caller's owners; live-server abandonment returns an ABORTED
  receipt, while closing does not manufacture business results.
- assignInline is the single inline construction/update entry; copyInline is
  removed without a forwarding shim. The fixed 128 B public inline ABI no longer
  uses a consumer macro/CMake option. Consumers must rebuild against the new API.
  Message default construction does not clear the inline tail, inline moves copy
  only initialized length, and owned moves transfer only metadata. Owner spans
  remain a caller-proven lifetime/extent precondition.

R2-0 proofs and interleavings are in README. The legal requester-close/responder-pop
race is tested by pausing the responder after observing an open Link: requester
reset returns without waiting, Ring remains allocated, and the pop finishes safely.
Request disposal then precedes responder reset. Tests no longer exercise the now
prohibited behavior of keeping ReceivedRequest past its Responder's lifetime.
Final role destruction with an unread request and unread reply releases both owners
exactly once, without collect or message consumption, in either role exit order.

Accepted space/lifecycle limits: closed Links and queued owners remain for the
entire service epoch, so short-lived requester churn grows memory until final role
exit. Stable names remain until Core exit and name lookup is linear. Final role
destruction and disconnected payload disposal may run arbitrary large deleters and
must be cold operations. IPC creates no thread, scheduler or business cancellation.

## Build and verification commands

Environment: Windows 11 build 26200, VS 2022 x64 MSVC 19.44.35226, CMake 3.31.10,
Ninja; i9-13900H, 14 physical/20 logical cores. Same isolated lux-cxx dependency
SDK as the baseline: `E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx`.
The archived run.ps1/production.ps1 record complete configure/environment arguments.

```text
cmake -DLUX_SOURCE_DIR=<exact-I-clone> -P <exact-I-clone>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <exact-I-clone>/cmake/render-v2-bootstrap -B <build> -G Ninja
  -DCMAKE_BUILD_TYPE=<RelWithDebInfo|Debug> -DBUILD_TESTING=ON
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx
  -DCMAKE_CXX_FLAGS="/EHsc [ASan: /fsanitize=address /Zi]"
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1
py -3 <exact-I-clone>/modules/platform/ipc/test/check_r2.py --root <exact-I-clone> --clean
```

ASan environment: `ASAN_OPTIONS=alloc_dealloc_mismatch=1`. Each affected ASan TU's
actual compile command contains `/fsanitize=address`; this is not inferred from a
build-directory name. ASan is not a race detector.

| Independent configuration | Result |
|---|---|
| RelWithDebInfo | all build, 24/24 CTest, second build no work |
| Debug | all build, 24/24 CTest, second build no work |
| RelWithDebInfo + ASan | all build, 24/24 CTest, no ASan diagnostics, second build no work |
| BUILD_TESTING=OFF | all build, second build no work; fresh relocated IPC and IPC+Protocol consumers 2/2 |

The 24-test sets retain the existing Core/Data/Protocol checks plus the nine IPC
groups. IPC directory, grace, FIFO, independent progress, protocol, ownership,
cost and installed consumer groups remain; R2 cases extend the existing groups.
No timeout was accepted as cancellation or success. No test was disabled.

Controlled directory tests retain 8-thread first creation and concurrent churn;
grace tests cover raw Channel read versus final release and the last-reader/retired
batch republication race. The terminal-publication/close race is still covered.
100,000 FIFO requests complete exactly once. In the independent RelWithDebInfo
progress run, A pauses 1.5 s while B completes 2,447,383 requests and the mock Tick
advances 8,343,927 times; A also progresses after resuming under B's flood. These
counts describe the mock service, not GPU/rendering performance guarantees.

## Allocation, movement and paired timing

Message/Reply sizes remain 160/328 B, inline capacity fixed 128 B. Counting DLL
move hooks verifies two standalone moves copy 16 B for an 8 B input, 256 B for a
128 B input, and zero inline bytes for owned input. A direct 8 B IPC roundtrip has
six message moves and 48 copied inline bytes. No uninitialized tail is read by
the implemented move path. Moved-from messages are empty and owners destroy once.

Source-level atomic audit for a stable successful one-request/one-reply cycle:
old receive/reply performs eight lifetime/access RMWs (two enter/leave pairs plus
four message-reference updates), and an empty pending-view check exchanges once.
R2 performs zero lifetime/access RMWs; it loads pending first and exchanges only
when adopting newly published Links. Underlying SPSC publication atomics remain
unchanged. This is an explicit source-operation count, not a hardware PMU claim.

EXE and DLL allocation counters remain zero in warmed 8/32/64/128 B paths and
complete small operations, including the responder worker. Generic G2 encodeWire
separately still reports two allocations. No claim is made that all serializers
or future RenderClient calls allocate nothing.

Freshly built baseline d29b6d44 and exact I use identical compiler flags, machine,
payloads and CPU topology. Three alternating before/after pairs preserve 10,000
samples each (30,000 per cell); all raw samples, maxima and attempt order remain.
Complete operation includes request SlotMap lifecycle, field/envelope encoding,
source routing, operation lookup/processing, terminal encoding, decoding and
correlation validation. It is a fixture, not a G8 Client API. Preencoded queue
cycling is recorded separately and is not substituted for full operation cost.

Primary matched run, nanoseconds, each cell p50/p95/p99:

| Path | Baseline | R2 |
|---|---:|---:|
| Complete operation, same thread | 400/500/600 | 200/500/600 |
| Complete operation, two physical cores | 1300/1700/1800 | 800/1400/1600 |
| Field codec 8 B, same thread | 300/500/600 | 200/300/400 |
| Field codec 32 B, same thread | 500/600/700 | 400/500/700 |
| Field codec 64 B, same thread | 800/800/900 | 600/800/1000 |
| Field codec 128 B, same thread | 1200/1300/1700 | 1000/1100/1200 |
| Field codec 8 B, two cores | 1100/1400/1500 | 700/800/1200 |
| Field codec 32 B, two cores | 1300/1500/1600 | 1100/1200/1300 |
| Field codec 64 B, two cores | 1500/1700/1900 | 1300/1400/1500 |
| Field codec 128 B, two cores | 2100/2400/2500 | 1700/1900/2000 |
| Preencoded queue, same thread | 200/300/400 | 100/200/1000 |

The queue p99 increase was isolated to one run. A second three-pair diagnostic
pins the main thread before executing any benchmark, keeping the service thread
on a separate physical core (masks 1 and 4). Queue p99 then equals 300 ns in both
versions; complete operation becomes 400/500/500 -> 200/300/500 ns on one thread
and 1300/1700/2000 -> 800/1000/1100 ns on two cores.

**Unresolved performance gate:** in that diagnostic, pooled 128 B single-thread
codec results are 1400/1500/1500 -> 1200/2100/2400 ns. R2 run 0 has a contiguous
slow interval around samples 4,500-7,500; its p99 is 2,500 ns versus 1,400/1,300 ns
in the other two R2 runs. All three initial matched runs instead improved that
case's p99 (1700/1700/1400 -> 1200/1300/1100 ns). This is non-stationary and not
consistently reproducible, but its specific cause is not established. No OS
scheduling/frequency attribution is claimed without evidence. Accordingly the
tail-latency gate remains PARTIAL rather than deleting this run or asserting a
universal speedup. Next investigation is a controlled cycle/context-switch profile
of this 128 B path; no speculative production workaround is included.

Large 1 MiB/256 MiB inputs retain backing addresses and exact single destruction.
Representative first matched R2 run: handoff 900/3800 ns, final release
49,100/8,500,400 ns. Allocation/initialization is outside handoff timing, and final
release runs on the requester caller. These are single observations, not percentile
claims. Raw large-message outputs from every run are archived.

## Protection, installation and limitations

All 719 frozen Legacy blobs match frozen/current/worktree identities. Every
baseline tracked file outside IPC is unchanged, including Core/Data/Protocol,
CMake guards, historical reports and user settings' committed version. The user's
uncommitted settings remain byte-identical, SHA256
`f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`.
Only that user change remains in the implementation worktree after I/V.

Each fresh relocated SDK has exactly 12 expected headers with matching source or
generated bytes. Public headers compile independently, fixed sizes are asserted,
and the installed interface no longer exports LUX_IPC_INLINE_CAPACITY. IPC's public
dependency is lux-cxx compile_time and its private SPSC dependency is concurrent;
Protocol remains independently consumable without IPC. Compile-command/include
audits find no Render/Legacy/Vulkan/ECS/Editor dependency in IPC production code.
All 20 test-build TUs and five production TUs use C++20 and /GR-.

Three headers in each Debug/RelWithDebInfo/Android include prefix were backed up
and synchronized (nine entries). install-sync.json records previous/new hashes
and actual backup paths. Android header sync is not Android build verification.

Root TOOLCHAIN was reconfigured and still fails on missing render/client,
render/features/cmake/feature_client.cmake and render/standard_content, followed
by their missing install-target errors. It is BLOCKED; no placeholders or hidden
legacy dependency were introduced. See root-closure.txt for the actual command
and failure. GPU/Validation/SyncValidation: N/A. Linux/Android/TSan: NOT_RUN.

Verification harness issues preserved in evidence: fetching the unrelated frozen
Legacy commit emitted a shallow-root warning but supplied the required objects;
all blob checks passed. Repeated VS shell setup emitted command-length warnings;
actual configure/build commands and ASan flags were checked and succeeded. The
first fixed-thread-affinity harness attempt lacked a thread query permission:
its suspended child was terminated before collecting samples, the harness handle
rights were corrected outside the source clone, then all six runs completed.
These are not hidden source fixes. Historical IPC PASS remains unchanged, while
this R2 delivery is explicitly PARTIAL pending the performance question above.
