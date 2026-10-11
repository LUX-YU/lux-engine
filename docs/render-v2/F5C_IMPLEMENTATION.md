# RD2-F5-C implementation and impact

Baseline V: `28bff67f10366eaa5febd25d0be33e7d13ced6dd`;
production I: `e662ff59844cf69f00d94341768df58ddc7f44b0`.
Scope is F5 closeout only. F4 cold performance debt stays PARTIAL under the
user's limited deferral. F6/F7 are NOT_AUTHORIZED. This document records
development evidence; independent qualification is a separate commit.

## Builder transaction

`Runtime.cpp::finish()` previously moved both unique owners before CreateThread;
failure destroyed them while leaving Assembly present. It now captures the
Win32 error, moves Backend and Transport back, then destroys temporary packets
and Event/Timer handles. No worker exists on this branch. Success still hands
off exactly once. Event/Timer failures already precede owner transfer.

`HostFaultRuntime.cpp` compiles the actual Runtime.cpp with four test-only Win32
call substitutions; the production TU and public ABI have no injection switch.
`HostFailures.cpp` exercises all three failure points, successful retry, move,
partial destruction, backend identity, handle balance and zero surviving workers.
Development run: PASS, 15 handles created/closed, validation errors 0. Borrowed
Backend identity survives failure. Moved-from/consumed Builder remains subject
to its existing moved-from precondition; a failed finish is **not** consumed.

## Physical storage: actual demand, same authority

Before changing allocation, the real footprint fixture captured these values
(capacity 8 Views, one imported RGBA32F output plus native host readback):

| Extent / FIF | Actual Views | Old slots | VMA allocation bytes | VMA block bytes |
|---|---:|---:|---:|---:|
| 1920x1080 / 2 | 1 or 4 | 16 | 1,097,072,640 | 1,304,428,544 |
| 1920x1080 / 3 | 1 or 4 | 24 | 1,645,608,960 | 1,855,979,520 |
| 3840x2160 / 2 | 1 or 4 | 16 | 4,262,461,440 | 4,262,461,440 |

These are allocation bytes, not a claim about physical residency. Logical payload
per slot is width * height * 16 for the image plus the same for readback; alignment
and driver memory requirements explain the allocation difference. Reserved blocks
are reported separately. Original executable/source and all rows are preserved in
`D:/LuxQualification/render-v2-f5c/before/` and `footprint-before-*.log`.
The initial diagnostic misspelled the descriptor-set count key; its summary zero
is invalid, not zero allocations. Its per-operation `descriptor_set` rows contain
the actual 16/24 counts; the final fixture fixes the summary key.

`Backend.cpp` extracts the existing import-target allocation into a cold free
function. A new variant initially allocates one View's FIF slots. A private
`RuntimeViewState::invocation_first` selects its actual slot group; global SlotMap
indices no longer dictate native storage. An unused group can be reused only
after every real slot completion. Otherwise cold assembly constructs FIF new
targets and calls `NativeGraphInvocations::append()`.

`append()` constructs a complete native candidate against the same immutable
code and same sampler catalog, then moves its slots into the existing owner.
Existing receipts, resource-vector backing addresses, descriptors and Scene donor
borrows remain stable. No queue wait, shader/pipeline/layout creation or hot growth
is added. Device/capacity/sampler errors and candidate failures leave existing
slots untouched. Runtime bounds total slots per variant by Views * FIF; cached
high-water storage is retained, not promised to shrink after each deletion.
The existing bounded variant cache and GPU Retirement still own eviction.

The first post-change 1080p/2-FIF measurement had 2 slots / 137,134,080 allocation
bytes for one View, 8 slots / 548,536,320 bytes for four Views, one native compile
and one Pipeline. A half-size fifth View used a second variant with 2 extra slots.
These are specific workload facts, not a general optimization percentage.

Resize reserves distinct candidate groups before committing View mappings. A
failed cold transaction may leave a valid unused cache entry, bounded by the same
variant/slot capacities, but cannot publish a partial View or overwrite its last
output. Failed native growth destroys its uncommitted targets/scratch/descriptors.
`Footprint.cpp` verifies partial Image and DescriptorPool failures against actual
VMA bytes/count, deletion/reuse, old-output preservation, repeated resize/cache
eviction and pixel readback. Existing path-switch and multi-Target oracles remain.
`NativeInvocations.cpp` now appends while the first real submission is accepted,
then reuses its Scene donor and receipt in the existing 2/3-FIF, four-View oracle.

## Import scale and minimal optimization

New generated `F5CReadBuffer` is a test-only consumer of the existing generator,
not a new parser/schema authority. ImportScale uses 1/8/32/64 real buffers with
two independently declared ranges, 1/4 invocation consumers and FIF 2/3; all
readback words are independently checked. It times production preparation through
the existing profiling wrapper. Zero allocation is checked around actual submit.

Before optimization, 64 imports / 4 consumers / 3 FIF measured preparation
p50 567,800 ns, p95 663,500 ns. Even with all prior work completed, each current
cell rescanned every previous slot/cell. The only production change in native
`graph/Frame.cpp` computes one local `has_pending_cells` from the **exact existing
cell tickets and queue completion values**. If false, repeated historical scans
are unnecessary; if true, the previous checks execute unchanged. Initial imported
tickets and Scene donor tickets are included. No cached revision, registry, new
persistent index or replacement synchronization proof is introduced. Empty-import
graphs skip this precheck. Worst-case pending-work complexity is not claimed fixed.
All before/after raw timings remain external; this diagnostic is not the formal
F4-C-P versus F5-C hot qualification.

## Bounded lifecycle review

- Removal and path/resize change release semantic View references; actual native
  slots retain their own accepted queue tickets. Slot reuse and variant retirement
  continue to use actual completion, including external-use joins.
- The one backend thread admits the aggregate Native and Presentation submission
  budget before executing a frame. Presentation's source release, graphics join,
  copy and retention are included. It has no competing queue submitter. No new
  queue or scheduling domain was introduced.
- Acquire fences, acquire-consumption tickets and Present fences remain separate.
  Errors other than capacity/busy stop the Runtime; out-of-date/surface-lost marks
  the target unavailable. Final WSI teardown retains its existing native waits.
  This audit is not proof of untested platform or driver-failure branches. Existing
  Win32 dual Target, resize/minimize and device-loss injection tests remain required.
- No WSI production change is made without a demonstrated defect. Hardware device
  loss and other platforms remain NOT_RUN. BGRA8/FIFO/maintenance1 and 1:1 relative
  extent boundaries are unchanged.

## Type responsibility and dependency impact

No new production class or owner is introduced. `invocation_first` is a private
value selecting native slots, not a second storage registry. The two new Backend
free functions allocate candidate target storage and select completed/free groups.
`append` is the one new public cold owner operation, consumed by Runtime and the
native lifetime test. It requires serialized CPU access and compatible sampler
borrows. GPU work can remain in flight because existing backing addresses survive.
Public header synchronization targets Debug, RelWithDebInfo and Android include
prefixes, with before/after hashes outside source; it is not SDK/Android qualification.

Affected production files: Runtime `Runtime.cpp`, `Backend.cpp`, `Frame.cpp`,
private `RuntimeBackend.hpp`; Vulkan Graph `Compile.cpp`, `Frame.cpp`, public
`Executable.hpp`. No Foundation allocation/queue/Sync2/Shader implementation,
F2 compiler, Core, Transport, generator, FINAL/RD2 or Legacy changes.

Development tests follow these actual dependencies: Runtime host-failure source
inclusion; native compile/storage + invocation/donor oracle; Runtime resize/History/
Presentation consumers; native preparation + import/alias/queue regressions.
Final I requires one complete Normal and Full ASan build/test/closure qualification,
original 168 obligations retained, plus new tests. `verify_f5c.py` checks File API,
compiler dependencies, generator jobs, link maps, protected Git objects and 719
Legacy entries. Historical evidence is referenced, never relabelled as rerun.

## Remaining gates

The independent report must supply final memory data, all tests/ASan/GPU status,
dependency closure, protected-user-file checks, fixed hot experiment and precise
I/V. Until then F5-C is NOT_QUALIFIED. The prior F5 p95 interval
[-3.6199%, +7.9439%] and F4 cold debt remain PARTIAL. The preregistered protocol
permits a truthful PARTIAL if the new experiment cannot resolve the 5% gate.
Product remains EXPECTED_UNAVAILABLE; no F6/F7 entry is implied.
