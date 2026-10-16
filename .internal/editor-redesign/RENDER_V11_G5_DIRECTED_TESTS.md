# G5 directed GPU completion and LastUse tests

This is a test contract and implementation map, not a PASS report. Execution logs and qualifications belong to the
exact implementation commit and its independent verification archive. Historical reports remain unchanged.

## Queue completion

`modules/function/render/native/vulkan/test/QueueCallbacks.cpp` uses actual Vulkan command buffers, submissions and
Fence queries. The existing instrumented Native seam only delays observation or injects a named native error.

- Callback quota is reserved before Submit. Dispatch disarms a node but retains its reservation until reset/destruction.
  A multi-chunk owner can rearm without another fallible capacity check.
- `poll`, `begin` and `wait` never invoke a callback. Even an already-completed Ticket waits for `dispatchCompleted`.
- Reverse registration and multiple subscriptions to one Ticket follow the actual queue frontier. Repeated dispatch
  cannot repeat a callback. A self-rearmed completed Ticket waits until the next dispatch snapshot.
- The callback may destroy its own unarmed node. It may not destroy another armed node or the queue owner.
- A later failed Submit does not replace an earlier accepted Ticket. Timeout and a transient Fence query failure do not
  prove GPU completion. Foreign queue/device Tickets cannot arm the local reservation.
- DeviceLost is an explicitly labelled observation injection. The test first drains the real native queue without
  advancing the library frontier, then checks error-only callbacks. It does not claim a physical GPU reset occurred.
- Real alias, same-family and dedicated queue configurations verify same-queue maximum merge and independent conditions
  for distinct physical queues, including equal numeric serials. Actual family/index and owner count are printed.

## LastUse interleavings

`modules/function/render/native/vulkan/test/LastUse.cpp` reuses `TextureSceneFixture.hpp` and its existing Graph, Shader,
compiler, prepared Texture binding and draw/readback path. It does not add a replacement renderer or shader.

Single chunk:

1. Upload and publish the old Texture, prepare its actual sampling Context, and drain unrelated queues.
2. Submit Region Clone/Copy T1. Hold completion observation and submit Draw T2 against the old backing.
3. Permit the graphics frontier to reach T1 only. Dispatch its completion and publish the replacement, releasing the
   old future-use lease through the normal Texture binding update and the upload's original source lease.
4. Confirm old image/view/sampler and its VMA range remain resident, with T2 still unobserved. Do not retain an extra
   `acquire(old)` lease, which would conceal the LastUse defect. Save old identities by value, not a stale pointer.
5. Complete T2 and collect once; confirm exact destruction, descriptor-capacity recovery, and old/new pixel values.

Multiple chunks use a different sequence, not merely a larger single-chunk input:

1. Submit the first Clone/Copy T1, then Draw T2 against the old backing.
2. Observe T1 only; block the second real CPU preparation through the existing CpuFixture hook.
3. Read the backing requirements through the existing `NativeMemoryAudit` friendship and prove T2 is still present.
4. Cancel, release the CPU barrier, and prove no T3 was submitted, last-good remains unchanged and completion is stopped.
5. Release the business identity and Context while T2 is still unobserved. Verify backing/descriptor preservation, then
   complete T2 and check exact reclamation. The direct requirements check is essential: Context retirement also retains
   a lease, so this release case alone would not prove that LastUse did not regress.

The Region implementation currently uses the graphics queue in every topology. Tests report that fact; full uploads
and the existing Transfer suite prove actual transfer/acquire behavior on the other queues. No dedicated-queue Region
claim is made. There is no separate Arena implementation: the physical allocation evidence is the DeviceSession/VMA
memory/offset/size plus actual image destruction and retained bytes. An unimplemented Arena cannot receive PASS.

## Shared pending state and scope

GPU callbacks execute at the RenderThread's explicit dispatch safe point and may finalize the existing pending slot
directly. They do not need another MPSC hop. CPU callbacks retain the existing bounded CompletionPost event path.
Runtime tests must continue to prove one original Request and one slot across all CPU/GPU chunks, one terminal Reply,
and no second resource/request owner. Readback must also leave the per-tick completion-discovery scan.

The historical 79 CTest names remain. Existing Texture upload tests migrate their real behavior to the generic
`render::upload`/StorageTraits path; a test-only copy of the deleted TextureUpload state machine would not qualify.
