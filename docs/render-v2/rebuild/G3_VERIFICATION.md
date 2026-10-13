# Draft4 G3 independent verification

**G3: PASS. Full product TOOLCHAIN closure: BLOCKED. G4: NOT_IMPLEMENTED.**

Date: 2026-10-13. Qualification covers the approved generic Transport, endpoint
routing, CPU byte ownership and terminal Reply contracts. GPU, Vulkan Validation
and SyncValidation are N/A. Linux and Android builds were not run.

## Exact identity and independent source

| Item | Full SHA |
|---|---|
| Implementation I | `c38315d0071c1ca4e10e04c6ff35a40ebcedfb62` |
| I parent / G2 Verification | `b3c0943ea157d66746601ff58bf6d4f46333ceb2` |
| Remote codex/render-v2 observed | `c221b93d1a8eccf690c5e4e0b848150f3634fd8b` |
| Frozen Legacy source | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| lux-cxx | `bf779515a120350c7c5412c366eea73afdf59ccf` |
| lux-cmake-toolset | `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60` |

V is the report-only commit containing this file, with exact I as its parent. Its
full SHA is supplied in the delivery message and external delivery.json; embedding
a commit's own hash in its contents would be self-referential. The local branch
remains `codex/render-v2-rebuild`. No push, merge or next-stage implementation.

Independent source: `E:/RenderV2Rebuild/G3/verify-final/source`, created using
`git clone --no-hardlinks --single-branch --branch codex/render-v2-rebuild`, then
detached at exact I. Frozen Legacy objects were fetched by SHA from the original
repository. Git reported its shallow-root warning; the required commit/tree/blob
objects were readable and all comparisons succeeded. HEAD remained exact I.

ValidateTrackedSnapshot passed before configuration. All **2,985 tracked files**
were SHA256-snapshotted before building and remained byte-identical after normal,
ASan, installed-consumer and root TOOLCHAIN checks. The clone stayed clean. No
production, test or build repair was made inside the verification source. Only
`final-*` evidence belongs to this final I qualification; earlier candidate logs
are retained separately and are not substituted for final-I tests.

## Actual results

| Gate | Result |
|---|---|
| Core/Data/Transport Windows C++20, no RTTI, all targets | PASS, 21 translation units / 28 Ninja actions |
| Normal second all build | PASS, `ninja: no work to do` |
| Normal CTest | PASS, 28/28, 7.83 seconds |
| AddressSanitizer all targets and CTest | PASS, 28/28, 9.24 seconds |
| ASan second all build | PASS, `ninja: no work to do` |
| Unchanged G1/G2 regression tests | PASS, all original 14 tests preserved |
| G3 tests | PASS, 12 behavior/concurrency groups + allocation + installed consumer |
| Public header isolation | PASS, 3 Core + 2 Data + 4 Transport headers, source and installed TUs |
| Fresh relocated installed consumers | PASS, Core/Data/Transport under both normal and ASan: six consumers |
| Installed canonical header set | PASS, exactly 12 source-matching headers in each of six fresh prefixes |
| Source and installed dependency interfaces | PASS, exact link assertions and actual compile command checks |
| Warm transport allocation | PASS, 6,000 preencoded cycles, zero global new/new[] allocations |
| Separately measured G2 encode | 2 allocations for the measured eight-byte fixture; not claimed allocation-free |
| Legacy and historical protection | PASS, 719 frozen/HEAD/worktree blobs, guard and old files unchanged |
| User settings | PASS, original bytes and binary patch preserved |
| Three-prefix public-header synchronization | PASS, 4 headers x 3 prefixes; all 12 destinations previously absent |
| Root TOOLCHAIN configure | BLOCKED, exit 1: later-stage consumers are missing |
| Full product build / CTest, Editor / Player | NOT-RUN because root configuration failed |
| GPU / Vulkan Validation / SyncValidation | N/A |

ASan used `/fsanitize=address /Zi`, with `ASAN_OPTIONS=alloc_dealloc_mismatch=1`.
Installed consumers inherited ASan flags. No suppression, disabled test or adjusted
historical verdict was used. ASan is memory-safety evidence, not a data-race detector.
Concurrency tests use barriers, bounded workloads and 45-second CTest timeouts;
timeout is failure rather than normal cancellation.

## Contract coverage and ownership review

| Test group | Established behavior |
|---|---|
| routes | Copied temporary metadata; duplicate rejection; exact Payload/Reply binding; deletion/clear/reuse; stale and foreign keys/tokens; old endpoint binding rejection |
| batch | Dispatch FIFO; middle failure does not skip later records; one terminal per item; Reply and SPSC queue pressure; no partial publication and recovery |
| replies | Deferred use prevents slot reuse after abandon; duplicate completion rejection; stale ticket cannot read replacement; malformed reply becomes Failed; omitted completion becomes Cancelled; typed late Reply survives receiver destruction |
| bulk | Counted owner survives original source destruction, queue, dispatch, defer, abandon and stop; repeated attachments conservatively charged; final controlled use destroys backing exactly once |
| generation | Exhaust the same lux-cxx SlotMap/StableSlotMap algorithms with tiny generation widths; no stale revival; exhausted prepared slots reject insertion |
| spsc | Wrong producer/consumer rejected; 2,000 ordered cross-thread command dispatches with terminal consumption and wake checks |
| mpmc | Four producers, three consumers, 1,600 unique records processed exactly once; all byte budgets returned |
| races | 100 complete/consume/abandon races, 100 complete/stop/abandon races, 100 Admission/stop races; single terminal/client winner and no stale-slot contamination |
| ordering | Deliberate cross-lane reverse order; explicit prerequisite-success ordering; all three lanes make progress under host servicing; deadline, stop-token and endpoint-stop waits exit |
| edges | Partial Reply reservation rollback, moved packet safety, truncated Wire, attachment limit, deferred byte saturation/recovery, bad reply length terminal, Bulk queue pressure independent of budget, stopped queued void operation settlement |
| owners | Input and discarded Reply destructors reenter Inbox safely; exact destruction counts prove user cleanup occurs outside locks |
| active-stop | Barrier holds a real handler across stop; terminal cancels immediately while input and budget remain live until handler return; drained waits for actual use |
| allocation | Preencoded payload/reply, reusable batch/output, all three lanes; publication, dispatch, terminal consumption and slot reuse allocate no heap after warm-up |

The new RouteTable SlotMap uniquely allocates OperationId. The shared state's
StableSlotMap uniquely allocates RequestId and owns Reply authority. Both are
direct lux-cxx SlotKey aliases. BoundOperation carries the verified typed contract
through its template types plus the private owner token/key; it has no extra
generation counter. Endpoint tokens retain instance identity across old bindings.
No hash container or independent identity allocator was introduced.

The receiver owns dispatch/stop responsibility. Sender, Inbox, Request, Promise
and consumed Reply retain shared communication state rather than a raw receiver
pointer. Queues hold RequestIds; persistent request auxiliary backing holds G2
OwnedWireBytes, attachments and the Bulk BudgetReservation. Terminal/client-use
and executor-input-use are separate lifetimes in the same slot. A slot is erased
only when both uses end, so cancellation or unread-reply release cannot invalidate
an active handler's input. There is no second mutable Reply store.

The Promise is the deferred input-use lease. Arbitrarily retaining an input byte
copy without retaining its Promise does not transfer Transport budget ownership
and is outside the public contract. Handler contexts and enabled lane servicing
remain host responsibilities. This implementation uses short locks and does not
claim lock-free communication or a fairness scheduler.

Transport reuses the R2 SPSC ring and bounded mutex/head/count MPMC algorithm,
lux-cxx AdmissionGate and BudgetGate. R2 native-layout payload, Lane-specific
operation identity and reply-free writes are deliberately obsolete contracts;
their replacements and test mapping are recorded in G3_IMPLEMENTATION.md. Old
source/tests/reports remain available unchanged in Git history.

The exact Transport interface dependency is:

```text
render_transport -> lux::engine::render::render_data
                 -> lux::cxx::container
                 -> LINK_ONLY lux::cxx::concurrent
```

Core/binary/memory dependencies remain the existing Data/Core closure. The isolated
dependency SDK has **64** source-matching lux-cxx headers. Source and installed
compile commands contain no Vulkan, Legacy, old installed Render SDK, Runtime,
ECS or Editor paths. Platform waiting details are private, and all four new public
headers compile without Windows/Vulkan macro leakage.

## Reproduction and evidence

`E:/RenderV2Rebuild/G3/run.ps1` loads VS 2022 amd64, MSVC 19.44.35226,
CMake 3.31.10 and Ninja. The final-I commands were:

```powershell
$source = 'E:/RenderV2Rebuild/G3/verify-final/source'
$build = 'E:/RenderV2Rebuild/G3/verify-final/build'
$asan = 'E:/RenderV2Rebuild/G3/verify-final/asan'
& E:/RenderV2Rebuild/G3/run.ps1 -Source $source -Action snapshot
& E:/RenderV2Rebuild/G3/run.ps1 -Source $source -Build $build -Action configure
& E:/RenderV2Rebuild/G3/run.ps1 -Source $source -Build $build -Action build
& E:/RenderV2Rebuild/G3/run.ps1 -Source $source -Build $build -Action build
& E:/RenderV2Rebuild/G3/run.ps1 -Source $source -Build $build -Action test
& E:/RenderV2Rebuild/G3/run.ps1 -Source $source -Build $asan -Action configure -ExtraFlags '/fsanitize=address /Zi'
& E:/RenderV2Rebuild/G3/run.ps1 -Source $source -Build $asan -Action build
& E:/RenderV2Rebuild/G3/run.ps1 -Source $source -Build $asan -Action build
& E:/RenderV2Rebuild/G3/run.ps1 -Source $source -Build $asan -Action test
py -3.13 E:/RenderV2Rebuild/G3/audit.py build
py -3.13 E:/RenderV2Rebuild/G3/audit.py asan
& E:/RenderV2Rebuild/G3/run.ps1 -Source $source -Build E:/RenderV2Rebuild/G3/verify-final/root-toolchain -Action root-configure
```

Configure expands to `cmake -S <source>/cmake/render-v2-bootstrap -G Ninja`,
RelWithDebInfo, BUILD_TESTING=ON, CMAKE_CXX_FLAGS=/EHsc (plus ASan when selected),
`lux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx`,
`lux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset`, package registries
OFF and module scanning OFF. Build is
`cmake --build <build> --target all -j 4 -- -k 0`; tests use
`ctest --test-dir <build> --output-on-failure -j 1`.

Audit invokes tracked `check_g3.py --root <source> --clean --build <build>
--installed <relocated-prefix> --user-root E:/SyncForder/CodeRepos/lux-engine
--snapshot E:/RenderV2Rebuild/G1/evidence/g0` on all six installed prefixes, then
checks actual consumer compile commands and every tracked source hash.

Evidence root: `E:/RenderV2Rebuild/G3/evidence/`:

- `final-snapshot.log`, `final-configure.log`, `final-build.log`,
  `final-no-work.log`, `final-test.log`, `final-asan-*.log`: final-I commands/results.
- `final-build-ctest.log`, `final-asan-ctest.log`: complete nested install, move,
  consumer build/test output and actual allocation probe numbers.
- `final-*-audit.json`, `final-*-compile-commands.json`, `final-*-ninja.log`:
  exact identities, Legacy/protection/header audits and compile inputs.
- `final-source-before.json`, `final-build-summary.json`, `final-asan-summary.json`,
  `final-summary.json`: all tracked file hashes and post-verification equality.
- `dependency-headers.json`, `design-inputs.json`, `include-sync.json`:
  authoritative input, dependency source and synchronized header hashes.
- `final-root-toolchain.log`: actual root configure failure and missing consumers.
- `final-user-protection.json`: original settings bytes and binary patch check.

Settings SHA256 remains
`f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`.
Public headers were copied from the qualified clone to Debug, RelWithDebInfo and
Android/lux-engine include prefixes. All 12 destinations were absent, so there was
no original content to back up; absence and resulting hashes are recorded for
every destination. Existing unrelated installed files were not removed. Shared
prefixes did not participate in module/consumer qualification. Android header
sync is not Android build qualification.

## Changes, remaining limits and stop

I changes 21 files: 20 new files and only one preexisting file (Render entry):

```text
docs/render-v2/rebuild/G3_IMPLEMENTATION.md
docs/render-v2/rebuild/check_g3.py
modules/function/render/CMakeLists.txt
modules/function/render/transport/CMakeLists.txt
modules/function/render/transport/README.md
modules/function/render/transport/include/lux/engine/render/transport/Operation.hpp
modules/function/render/transport/include/lux/engine/render/transport/Replies.hpp
modules/function/render/transport/include/lux/engine/render/transport/Routes.hpp
modules/function/render/transport/include/lux/engine/render/transport/Transport.hpp
modules/function/render/transport/pinclude/lux/engine/render/transport/State.hpp
modules/function/render/transport/src/Replies.cpp
modules/function/render/transport/src/Routes.cpp
modules/function/render/transport/src/Transport.cpp
modules/function/render/transport/test/Allocation.cpp
modules/function/render/transport/test/CMakeLists.txt
modules/function/render/transport/test/Concurrency.cpp
modules/function/render/transport/test/Contracts.cpp
modules/function/render/transport/test/Edges.cpp
modules/function/render/transport/test/Support.hpp
modules/function/render/transport/test/installed/CMakeLists.txt
modules/function/render/transport/test/installed/Main.cpp
```

V adds only this report. ALLOWED is Transport, Render exports and new G3 materials.
READ-ONLY is Core/Data, Legacy, history, dependency source and user settings.
FORBIDDEN is business Client/Server/Dispatcher, Feature, Graph, GPU, Legacy
linkage, placeholder targets, hidden dependency repair and disabled tests.

The root TOOLCHAIN still references missing `render/client`,
`render/features/cmake/feature_client.cmake`, and `render/standard_content`, then
fails exports for render_client/render_feature_client/render_standard_content.
This is the remaining product closure blocker. No G3 lifecycle, memory-safety or
data-correctness gate remains failed. Full historical F4/F5/F6 performance, Linux,
Android and GPU behavior are not newly qualified.

Route hot unload, RenderClient/Server, business completion semantics and later
GPU/Feature/Graph stages remain NOT_IMPLEMENTED. Future assembly must respect the
documented host thread/context and deferred Promise lifetime contracts. G3 is
complete; stop for user review without starting G4 or publishing the branch.
