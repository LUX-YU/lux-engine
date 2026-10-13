# Draft4 G3 Implementation

Parent: G2 V `b3c0943ea157d66746601ff58bf6d4f46333ceb2`.
Branch: `codex/render-v2-rebuild`. Scope: the user-approved G3 plan only.
Draft4/Implementation Plan remain authoritative; historical RD/R2 reports remain
unchanged. No push, merge or G4 work is part of this stage.

## Production changes and ownership

New static `render_transport` contains four installed headers, one private state
header and three implementation files. The existing Render entry only adds that
subdirectory and exported component. G1 Core and G2 Data production/tests are
unchanged. The Transport README defines threading, packet, reply and shutdown
contracts in detail.

| Object | Unique owner / identity |
|---|---|
| Operation descriptor and handler binding | RouteTable's SlotMap of stable owned RouteEntry; OperationId = SlotKey |
| Endpoint identity | Shared instance token, retained by old BoundOperation/ReplyTicket |
| Queue/Admission/Budget/Reply state | Shared TransportState, retained by receiver and outstanding communication objects |
| Request identity and terminal | Prepared StableSlotMap, RequestId = SlotKey; no second result queue |
| Input bytes, attachment owners, Bulk budget | StableSlotMap auxiliary backing, released by the unique queue/Promise work claim |
| Consumed reply bytes | TerminalReply; immutable route metadata retained through shared state |
| Handler application context and threads | Host; borrowed function/context, no library-owned worker |

State is concrete shared communication storage with lifecycle behavior, not an
extra forwarding Impl/Manager. Endpoint tokens carry no numeric Owner counter.
Queues are non-owning ID sequences; request slots alone hold bytes and terminal
state. There is no unordered_map/unordered_set, custom generation allocator,
PinnedRenderBytes, native payload ABI, public unbound submission or no-reply write.

Bulk leases persist after handler dispatch, success, cancel, abandon and stop until
the actual Promise input-use ends. Reply recycling waits for both input-use and
client-use completion. Owner destruction is outside locks. Generation exhaustion
retires slots according to lux-cxx rather than reviving an old key.

## R2 algorithm and test migration

Read-only source examined at historical commit
`c221b93d1a8eccf690c5e4e0b848150f3634fd8b` under
`modules/function/render/transport`. Nothing is copied back as a legacy directory.

| Historical mechanism/contract | G3 treatment and evidence |
|---|---|
| Transport.cpp PROGRAM/CONTROL Ring | Same lux-cxx BoundedSpscFrameRing and four persistent slots; warm capacity; batch/spsc/allocation tests |
| Transport.cpp UPLOAD mutex + head/count circular queue | Same bounded MPMC try-lock/index algorithm; queue now carries owner-assigned RequestId; mpmc/edges tests |
| Admission acquire/close/in-flight and host-driven drain | Reuse lux-cxx AdmissionGate, preserve stop/publication race settlement; races/active-stop tests |
| UPLOAD bytes returned at dispatch exit | Replaced by lux-cxx BudgetGate lease in request backing, extending through deferred use; bulk/edges tests |
| Reply capacity, duplicate/stale/foreign rejection | One prepared StableSlotMap plus short mutex transitions; replies/generation/races tests |
| ReplyLifetime late request/inbox/stop behavior | Shared state and endpoint token, published terminals survive receiver; replies/owners tests |
| Wake notification | Private C++20 condition_variable_any, epoch + deadline/stop predicate; ordering and spsc tests |
| Compile-time Lane operation restrictions | Obsolete: lanes select physical scheduling; same operation runs on all three, ordering/allocation tests |
| Reply-free mutation and native-layout Payload | Obsolete: every admitted item has a terminal; G2 typed wire validator/codecs and owned bytes |
| Old type hash/Owner number/static metadata borrowing | Replaced by copied full SchemaId metadata + SlotKey + shared endpoint instance |
| Historical microbench qualification | Preserved as history, not rerun or reclassified; new bounded allocation probe only |

No R4 GPU synchronization, pipeline/compiler or device algorithm is implemented or
changed. Completion labels do not assert their future GPU semantics are fulfilled.

## Allowed / read-only / forbidden

ALLOWED: `modules/function/render/transport/**`, Render entry/exports,
`G3_IMPLEMENTATION.md`, new `check_g3.py`, and report-only G3 V.

READ-ONLY: Core/Data, historical reports/checkers, Legacy 719 files + guard,
canonical error/description headers, sibling lux-cxx/toolset and user settings.

FORBIDDEN: RenderClient/Server/Dispatcher, Feature, Graph, Native, business handlers,
legacy linkage, dummy targets, hidden dependency fixes, disabled existing tests.

## Build and independent verification procedure

External workspace: `E:/RenderV2Rebuild/G3`. Dependencies remain:

- lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`;
- lux-cmake-toolset `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`.

The isolated SDK adds concurrent to the already qualified G2 dependency set;
64 lux-cxx headers match sibling source bytes. It contains no old Render headers.
Source and installed CMake assert the exact Transport link interface: render_data,
lux-cxx container and LINK_ONLY concurrent; memory/binary/core arrive through Data.

Use unchanged `cmake/render-v2-bootstrap`, Windows MSVC 19.44.35226, CMake 3.31.10,
Ninja, C++20/no RTTI and RelWithDebInfo, BUILD_TESTING=ON. Build all targets using
`cmake --build <build> --target all -j 4 -- -k 0`, repeat for no-work, then
`ctest --test-dir <build> --output-on-failure -j 1`. Repeat with
`/fsanitize=address /Zi` and `ASAN_OPTIONS=alloc_dealloc_mismatch=1`.

Implementation tests include unchanged G1/G2 tests, G3 contracts/concurrency with
45-second timeouts, allocation instrumentation and a relocated installed consumer.
Every new public header compiles alone in source and installed consumers. No test
depends on NDEBUG-sensitive assert for its verdict.

Commit I after diff/protection review. Independently clone exact I, run
ValidateTrackedSnapshot before configure, hash all tracked source before/after,
repeat build/CTest/ASan/install/closure checks, and run check_g3.py. Keep raw evidence
outside source; V adds only its report. Any implementation repair invalidates that
I qualification and requires a new I and clean verification.

Sync public headers to Debug, RelWithDebInfo and Android include prefixes from
qualified source with original-file backup and SHA256 records. These shared
prefixes must not satisfy bootstrap or installed-consumer verification.

Recheck root TOOLCHAIN separately. Its missing later-stage modules remain a real
product closure blocker, not a failing Transport module gate to hide. GPU/Vulkan
Validation/SyncValidation: N/A; Linux/Android builds: not run. G3 ends after I/V.
