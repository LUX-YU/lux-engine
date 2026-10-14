# G3-R-IPC independent verification

IPC qualification: **PASS**, within the documented single-logical-owner and cold-disposal contract.
Full product / root TOOLCHAIN closure: **BLOCKED**, separately from IPC.
G4/G8, Graph, Feature, Native, Engine adapters: **NOT_IMPLEMENTED by this stage**.
No push or merge is authorized or performed by this delivery.

## Exact source and evidence

| Identity | Value |
|---|---|
| Branch | `codex/render-v2-rebuild` |
| Implementation I | `19b82aa39b51fe9c38a03a75adca51a3aa8f7e6f` |
| I parent /施工 baseline | `05e4d385d59b05a02baa017bc05feb1e3543508b` |
| Actual remote branch HEAD, rechecked at delivery | `05e4d385d59b05a02baa017bc05feb1e3543508b` |
| Verification V parent | exact I above |
| lux-cxx | `bf779515a120350c7c5412c366eea73afdf59ccf`, clean |
| lux-cmake-toolset | `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`, clean |
| Frozen Legacy source | `a669409a289a6fa4092f21176397795b1cdb7f3e` |

This report is the only file in V. Its own commit SHA is obtained with
`git log -1 --format=%H -- docs/render-v2/rebuild/IPC_VERIFICATION.md` and recorded
in the delivery response and external `delivery.json`; embedding a commit's own
hash in its content is not possible.

Independent source: `E:/RenderV2Rebuild/IPC/verify/source`, cloned with
`--no-hardlinks --no-checkout --single-branch`, then detached at exact I.
`ValidateTrackedSnapshot.cmake` passed before configuration. All 2,994 tracked
file SHA256 values before and after verification match, and the clone is clean.
No production or test source was repaired in that clone.

Raw logs, CSV samples, protection manifests, commands and installation hashes are
archived outside the repository under:
`E:/RenderV2Rebuild/IPC/qualification/19b82aa39b51fe9c38a03a75adca51a3aa8f7e6f/`.
Working build/evidence trees are `E:/RenderV2Rebuild/IPC/verify/`.
`evidence-manifest.json` enumerates archived files and hashes. These local evidence
paths are not portable repository assets.

## Changes, owners and lifecycle result

I changes 26 files: the new IPC module and its tests; three CMake entries
(`modules/platform/CMakeLists.txt`, `modules/function/render/CMakeLists.txt`,
`cmake/render-v2-bootstrap/CMakeLists.txt`); two implementation/lifetime notes and
`check_ipc.py`; removal of six old `render/runtime/connection` files. The full
name/status list is `implementation-files.txt` in the evidence archive.

Added public types with real responsibilities: `IpcCore`, `IpcRequester`,
`IpcResponder`, `IpcMessage`, `ReceivedRequest`, `IpcReply`, `IpcQueueConfig` and
transport result/status values. The exported target is a real shared library,
`lux::engine::platform::platform_ipc`, namespace `lux::platform::ipc`.
Core identity and same-name responder exclusivity were verified across the EXE
and two DLL consumers. No renderer Client/Server or business ID allocator exists
in IPC. Render SlotKey identities and pure Protocol production files are unchanged.

Core owns immutable sorted name anchors, service-epoch Channels, Link metadata
and Rings. Roles and deferred requests hold participation rights. Each requester
uses its own Request and Reply SPSC Rings. A received request retains its input
and exactly one source reply right; it is not a central mutable RequestCell.
Messages uniquely own their erased holder/deleter or contain inline bytes.
Existing shared assets are retained through an exclusive holder without copying
their backing bytes. Original-module deleters run once after Channel destruction;
the host must keep that module loaded until the last owner is released.

The detailed linearization table and reclamation argument are in
`IPC_LIFECYCLE.md`. Controlled interleavings exercise name publication, protected
Channel acquisition versus last release, last-reader exit versus retirement
republication, Ring access versus reset, and terminal publication versus close.
Cold directory operations use conservative SC atomics; Ring publication retains
lux-cxx acquire/release behavior. A retired batch is detached before testing reader
count and republished/rechecked if readers remain, avoiding a missed final reclaim.
Last-use tests check actual Channel/Link/Ring destruction without another API call.

**Execution-context limitation:** role create/reset/destruction and final disposal
of disconnected deferred work are cold operations. Requester reset closes new Ring
access and waits for pre-existing short Ring borrows before destroying the Rings
and queued owners on that caller. It does not wait for business/deferred completion,
but can wait for a preempted borrower; reset is **not wait-free**. Normal send,
receive and reply paths have no mutex, condition variable, spinlock or wait. A failed
closed reply retains both input and result so the host can move their arbitrary
deleters to a cold disposal point. There is no hidden cleanup thread. A caller that
disposes arbitrary orphan payloads on its hot Tick would violate this contract.

Responder polling preserves a round-robin cursor, adopts at most one pending Link
and visits at most eight Links per call. A pending accepted request reserves reply
credit until reply/abort consumption. Historical returned replies can outlive the
Channel without holding credits. Published replies are drained before CLOSED;
the empty-observation/close race is covered by a second Ring check after CLOSED.
Transport ABORTED retains original opaque input and does not manufacture a Render
Cancelled result. An entire disconnected connection leaves unknown business
outcomes for its upper layer to settle.

## Commands and independent results

Windows 11 build 26200, VS 2022 x64 MSVC 19.44.35226, CMake 3.31.10, Ninja.
CPU: i9-13900H, 14 physical cores / 20 logical processors. See `cpu.json`.
Dependency SDK: `E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx`;
64 installed lux-cxx headers match the clean sibling source byte for byte.

The archived `run.ps1` contains the complete environment and configure commands.
Executed commands, with `<source>` the exact-I clone and `<build>` each directory
in `verify/{build,debug,asan,production}`, were:

```text
cmake -DLUX_SOURCE_DIR=<source> -P <source>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <source>/cmake/render-v2-bootstrap -B <build> -G Ninja
  -DCMAKE_BUILD_TYPE=<RelWithDebInfo|Debug> -DBUILD_TESTING=ON
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx
  -DCMAKE_CXX_FLAGS="/EHsc <ASan flags when applicable>"
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
  -DCMAKE_INSTALL_PREFIX=<build>/sdk
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1
py -3 <source>/docs/render-v2/rebuild/check_ipc.py --root <source> --clean --final
  --build <build> --installed <fresh-prefix>
  --user-root E:/SyncForder/CodeRepos/lux-engine
  --snapshot E:/RenderV2Rebuild/G1/evidence/g0
```

| Configuration | Build | Tests | Second build |
|---|---|---|---|
| RelWithDebInfo | PASS, 20 TUs | 24/24 PASS | no work |
| Debug | PASS, 20 TUs | 24/24 PASS | no work |
| RelWithDebInfo + ASan | PASS, 20 TUs | 24/24 PASS, no ASan diagnostics | no work |
| BUILD_TESTING=OFF production | PASS, 5 TUs | 2/2 external IPC/IPC+Protocol consumers PASS | no work |

ASan flags: `/fsanitize=address /Zi`; `ASAN_OPTIONS=alloc_dealloc_mismatch=1`.
CTest groups include all seven Core and seven Data tests, pure Protocol installed
consumption, and nine IPC groups. Tests have timeouts; none timed out. The five old
Connection tests passed alongside their new replacements before removal in I;
their historical reports and checker remain unchanged. Effective behavior is
migrated; old Listener, shared-pool, thread-ID and Render cancellation internals
are deliberately not retained as compatibility interfaces.

Verified: 8-thread controlled first creation and repeated epoch churn; 100,000
FIFO messages with exactly-once completion; duplicate responder, wrong source,
old epoch, reply credit exhaustion and publish rollback; requester-first NotReady;
opaque abort receipts; two-role shutdown before deferred disposal; exact owner
destruction; shared asset original-address retention; old equal-valued Render
RequestIds isolated by source; schema/version/layout failures; no hidden worker.
The tests use controlled hooks/barriers plus stress; ASan is not a data-race detector.

## Performance evidence and limits

The following are independent RelWithDebInfo observations, not threshold promises.
The DLL has allocation/test instrumentation enabled for these measurements.
10,000 raw samples per row; values are nanoseconds. Dual-thread runs are pinned
to distinct physical cores discovered from topology, affinity masks 1 and 4.

| Measured path | p50 | p95 | p99 |
|---|---:|---:|---:|
| Complete small operation, same thread | 400 | 500 | 500 |
| Complete small operation, two physical cores | 1,400 | 1,600 | 1,800 |
| Preencoded queue cycle, same thread | 200 | 300 | 300 |
| Field codec, 8 B, same thread | 400 | 600 | 1,500 |
| Field codec, 32 B, same thread | 600 | 700 | 800 |
| Field codec, 64 B, same thread | 800 | 1,100 | 1,200 |
| Field codec, 128 B, same thread | 1,300 | 1,400 | 1,400 |
| Field codec, 8 B, two physical cores | 900 | 1,700 | 2,200 |
| Field codec, 32 B, two physical cores | 1,300 | 1,700 | 1,800 |
| Field codec, 64 B, two physical cores | 1,600 | 2,000 | 2,200 |
| Field codec, 128 B, two physical cores | 2,000 | 2,700 | 3,200 |

The complete operation includes prepared SlotMap request identity allocation,
24-byte request envelope encoding, delivery, operation lookup and processing,
17-byte terminal encoding, reply consumption, identity/status validation and slot
recycling. It uses G2 scalar field encoding and a fixture-specific validated
envelope. It is neither a preencoded benchmark nor a production G8 Client API.
Ordinary generic G2 `encodeWire` still allocates: the separately measured example
performs two allocations. No complete generic-serialization zero-allocation claim
is made. EXE and IPC DLL allocation counters both report zero in the warmed inline
codec/operation paths, including the responder worker path.

Inline capacity 128 B was selected after comparison with 64 B. Message/reply sizes
are 160/328 B versus 96/200 B; default depth 64 costs 12,288 more Ring bytes per
Link but avoids two allocations for each 128-byte roundtrip. The 64-byte comparison
had 20,000 EXE allocations over 10,000 such operations. It is implementation-phase
selection evidence, not an independently requalified second ABI.

With A paused for 1,500 ms, B completed 1,792,577 requests and the mock service
advanced 711,822 Ticks. Reverse flood/resumption also passed. The first 100,000
Tick intervals have p50/p95/p99 1,700/6,400/7,000 ns (max 177,300 ns). These are mock
Ticks, not GPU rendering performance or a scheduling guarantee.

Large payload observations, one sample per size in each configuration: 1 MiB
handoff roundtrip 900 ns, final release 91,500 ns; 256 MiB 4,300 ns and 7,391,700 ns.
Allocation/initialization is outside handoff timing. Final release runs on the
test's requester/main caller after consuming the returned original input; address
identity and exactly one destructor are checked. These are not percentile data.
Cold create/reset separately has 100 samples: create p50/p95/p99 1,200/2,200/4,600 ns,
destroy 300/300/400 ns. Raw samples and all configurations are retained in the
archive; maxima/outliers are not discarded from those files.

## Installation, dependency and protection gates

Fresh-prefix installation and moving the prefix passed in all three test
configurations. Installed public headers compile individually; consumers use
`find_package` for pure IPC, pure Protocol and their combination. Production
BUILD_TESTING=OFF also installs and runs both external consumers without hooks.
There are 12 exact installed headers including generated visibility and required
Core support headers. All installed bytes match exact I/generated output.

IPC public interface links only `lux::cxx::compile_time`; SPSC concurrent support
is private. File API, compile commands, source includes and installed export tests
confirm no IPC dependency on Render/Legacy/Vulkan/ECS/Editor/stdexec/OS headers.
Protocol links Core/Data and does not require IPC. Only the IPC test fixture links
both. All 20 test-build TUs and five production TUs use C++20 and `/GR-`.

Nine header destinations were synchronized: IpcCore.hpp, IpcMessage.hpp and
visibility.h in Debug, RelWithDebInfo and Android include prefixes. All were absent
before this stage, recorded as null original hashes rather than fictitious backup
files. `install-sync.json` contains exact paths and resulting SHA256 values.
Android header synchronization is not Android build qualification.

Legacy's frozen, current committed and working-tree blobs agree for all 719 files.
All baseline files outside the explicit whitelist, including CMake guards,
Core/Data/Protocol and historical reports/checkers, retain their original Git blobs.
The only remaining user worktree change is `.vscode/settings.json`, unchanged from
its prior snapshot, SHA256:
`f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`.
`protection-{build,debug,asan,production}.json` records independent checks.

## Failures, exclusions and next boundary

The independent clone initially lacked the unrelated frozen Legacy Git object.
Fetching exact `a669409a289a6fa4092f21176397795b1cdb7f3e` from the original repository
supplied it; no source change was made. Protection checks then passed. This was
verification object availability, not a waived Legacy check.

Implementation work corrected the empty-reply/close race and a test's SlotKey
member spelling before I. Independent V required no implementation repair.
Some initial PowerShell transcripts retained native exit status without native
stdout; complete CTest LastTest logs, CSVs, CMake build metadata, explicit captured
no-work outputs and the captured root configure output are archived separately.

Root TOOLCHAIN was actually configured with the existing vcpkg toolchain and
RelWithDebInfo dependency prefix. It exits 1 because `render/client`,
`render/features/cmake/feature_client.cmake`, and `render/standard_content` are
absent, followed by missing `render_client`, `render_feature_client` and
`render_standard_content` install-target errors. See `root-closure.txt` and the
runner's exact command. No placeholder target, disabled test or old SDK Render
implementation was used to satisfy those missing modules. Root product build is
therefore BLOCKED and is not represented by the successful module bootstrap.

GPU, Vulkan Validation and SyncValidation: **N/A**. Linux, Android and TSan:
**NOT_RUN**. IPC has no GPU completion semantics. Generic Render pending outcome
settlement and production renderer clients remain future work, not IPC claims.
Cold borrower scheduling and caller-controlled cold orphan disposal remain the
explicit runtime constraints described above. No unresolved observed IPC test
failure remains. Next authorized boundary is still G4, requiring separate approval;
this delivery stops after V and does not push or merge.
