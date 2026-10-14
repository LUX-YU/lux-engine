# G3-R-IPC implementation

Baseline and parent: `05e4d385d59b05a02baa017bc05feb1e3543508b`, branch
`codex/render-v2-rebuild`. No push, merge, G4, G8 renderer or GPU work is included.
The exact qualified I/V identities and final results belong to IPC_VERIFICATION.md.

## Responsibility and lifetime

- `platform_ipc` is a shared component, namespace `lux::platform::ipc`, exported as
  `lux::engine::platform::platform_ipc`. One exported function-local Core is shared
  by EXE/DLL callers. Public dependency: lux-cxx compile_time (`expected`); private
  concurrent queue algorithms. No Render/Data, Native, ECS, task or log dependency.
- Core physically owns immutable name anchors, each service-epoch Channel, Link
  metadata and Rings. Roles are move-only participation rights. ReceivedRequest
  pins its epoch/Link and exclusively retains the input. There is no new public ID
  allocator: Render RequestId/OperationId remain unchanged SlotKeys in Protocol.
- Each requester owns use of one Request SPSC and one Reply SPSC, default depth 64.
  One outstanding request reserves one reply credit, returned only on consumption.
  Non-successful send retains input. Closed reply retains both work and output.
- Requester-first is NotReady. Responder claim is unique. Reset creates an epoch
  boundary; old roles/work cannot join a replacement responder. No thread IDs,
  explicit listen/connect/initialize, hidden thread, central RequestCell or hot
  shared queue is introduced.
- Cold requester reset closes Ring access, waits for short existing Ring borrows,
  then physically destroys both Rings on that caller. It never waits for business
  completion. Deferred input remains independently alive. Cold disconnected-work
  disposal releases the final pin; no subsequent collection API call is needed.
- Ordinary trySend/tryReceive/tryReply perform no waits. Cold reset is NOT wait-free;
  it may wait for a preempted short borrow. Arbitrary owned deleters must run at a
  caller cold disposal point, including when deferred work outlives both roles.
  See IPC_LIFECYCLE.md for the publication/reclamation argument and this precondition.
- An empty reply observation is rechecked after acquiring CLOSED, so a concurrently
  published terminal is not hidden by close. Unfinished work produces an IPC ABORTED
  receipt preserving opaque original bytes; it does not manufacture Render Cancelled.

## Message representation and measured scope

IpcMessage is inline bytes or one uniquely moved erased owner/deleter, not a shared
message pool. Existing shared immutable assets may be retained by an exclusive
holder. The allocating/deleting module must stay loaded; the DLL fixture tests the
original-module deleter after Channel death. No large asset copy or 16 MiB cap exists.

The selected default inline capacity is 128 bytes. The 64-byte comparison has
sizeof(Message)/sizeof(Reply) 96/200; 128 has 160/328. At depth 64, the latter costs
12,288 more Ring storage bytes per Link but avoids two heap allocations per 128-byte
roundtrip (20,000 allocations in the 10,000-operation comparison). Both support zero
hot DLL allocations. The build definition is exported to consumers; it is not a
permanent ABI promise, and changing it requires rebuilding all consumers.

Benchmarks separate preencoded queue cycling, field-codec payload sizes 8/32/64/128,
and a complete small operation: local SlotMap request creation, request/operation
envelope encoding, actual source delivery, route lookup, terminal encoding, terminal
correlation/validation and pending erasure. The operation uses the G2 scalar encoder;
its fixture-specific envelope uses BinaryReader/BinarySpanWriter. This is not a
production RenderClient API (G8), nor the generic allocating G2 encodeWire path.
Generic G2 encodeWire is exercised and its allocations reported separately.
Physical-core affinity is selected using Windows topology (not assuming CPU 0/1
are separate cores). Raw cold/create, queue, codec, operation and Tick CSVs are saved.
1 MiB and 256 MiB owner address, handoff and actual final disposal are measured.

## Changes and migration

Added: IPC public Core/roles/messages, one implementation TU, private testing hooks,
per-module allocation instrumentation, lifecycle/message/directory/grace/FIFO/progress/
Protocol/cost tests, two DLL consumers and relocated installed consumers.
Changed: Platform component registration, bootstrap source/install entry, Render
module entry. Removed: the six old runtime/connection files after equivalent tests
passed alongside the original 20-test qualification. No compatibility aliases remain.

The original G3-R connection tests passed before removal; their effective contracts
are replaced by IPC fixtures. Thread-ID checks, Listener lifetime exclusion, default
byte budget, shared encoding-pool exhaustion and business cancellation inside the
transport are superseded contracts, not preserved interfaces. Historical reports and
check_g3r.py remain byte-identical; check_ipc.py audits the new boundary.

## Verification entry points

Use the existing VS 2022 x64 developer environment, CMake 3.31.10 and Ninja. Dependency
SDK: `E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx`, checked against sibling
lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`; toolset
`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`.

```text
cmake -DLUX_SOURCE_DIR=<clean-I-clone> -P <clean-I-clone>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <source>/cmake/render-v2-bootstrap -B <build> -G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
  -Dlux-cmake-toolset_DIR=<toolset> -Dlux-cxx_DIR=<isolated-cxx-sdk>
  -DCMAKE_CXX_FLAGS=/EHsc -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1
py -3 docs/render-v2/rebuild/check_ipc.py --root <source> --clean --final --build <build>
```

Repeat Debug and ASan (`/EHsc /fsanitize=address /Zi`,
`ASAN_OPTIONS=alloc_dealloc_mismatch=1`). Tests invoke fresh relocated Core/Data/
Protocol/IPC consumers, including a combined installed consumer. Independent V also
checks BUILD_TESTING=OFF, actual target/compiler/export closure, tracked byte snapshots,
719 Legacy blobs, historical files and settings protection. Raw artifacts remain
outside the source tree. Root TOOLCHAIN must be attempted and its real missing future
consumers reported separately. GPU/Validation/SyncValidation N/A; Linux/Android/TSan
NOT_RUN. ASan is not a race detector. Final qualification is never inferred from this
implementation description or historical G3-R measurements.
