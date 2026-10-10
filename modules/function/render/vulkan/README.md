# Vulkan Foundation (R4)

`render_vulkan` is a real static native component. Public dependencies are Core
and the Vulkan loader/headers. VMA is private. There is no Graph compiler,
Runtime, Scene/View, thread, window, surface, swapchain or Feature executor.

## Ownership and public API

| Header | Owners / operations | Lifetime |
| --- | --- | --- |
| `Error.hpp` | Native VkResult error, invalid argument, unsupported, capacity, wrong owner, busy | Core structured error descriptors; no registry or output service |
| `device/Device.hpp` | `VulkanInstance`, `VulkanDevice`, `VulkanAllocator`, `selectQueueFamily` | Instance owns messenger; Device borrows native instance; allocator borrows device |
| `memory/Memory.hpp` | `Buffer`, `Image`, `EMemoryAccess`; read/write | VMA allocations borrow allocator/device; persistent mapping is VMA-owned |
| `descriptor/Descriptors.hpp` | `DescriptorSetLayout`, `DescriptorPool`; allocate/writeStorageBuffer | Pool owns all sets; returned set handles are explicit borrows |
| `pipeline/Pipeline.hpp` | `ShaderModule`, `PipelineLayout`, `ComputePipeline` | Device-borrowing native owners; compute entry is `main` |
| `transfer/Submission.hpp` | `SubmissionQueue`, `CommandBatch`, `SubmissionTicket` | Queue owns fixed command pool/buffers/fences; batch and ticket borrow the stable queue |
| `transfer/Transfer.hpp` | `StagingArena`, `StagingSlice`; buffer/image copy recording | Fixed VMA buffer, one atom-isolated partition per submission slot |
| `retirement/Retirement.hpp` | `RetirementQueue::retire/collect` | Fixed slots own transferred native backing until last-use completion |

Factories return `RenderResult<T>`, owners are move-only, destructors release
backing. A wrapper move preserves native identity. Destroying or move-assigning
an existing owner requires all its CPU/GPU borrows to have ended. A moved-from
owner may be destroyed or assigned; other operations require a complete owner.
`SubmissionQueue` is factory-owned by `unique_ptr` so tickets/batches retain a
stable borrowed address. There is no per-operation reference counting.

Parents outlive children: Instance → Device → Allocator → Buffer/Image. Native
descriptor/pipeline owners also borrow Device. Queue outlives its batches,
tickets, staging and retirement owners. Callback user data outlives Instance.
These are explicit C++ borrow preconditions, not a global ownership registry.
Tests observe balanced native create/destroy and ordering; the API does not
promise to detect every misuse of an arbitrary raw Vulkan handle.

## Native configuration and synchronization contract

The loader and physical device must support Vulkan 1.3. Only synchronization2 is
enabled. Extensions are explicit, enumerated and checked; there are no default
device extensions. Selection chooses a graphics+compute family and prefers an
eligible discrete GPU, or checks an explicit enumeration index. Presentation
support must be negotiated against the real future surface in R5; it is NOT_RUN.
Validation mode requires Khronos validation, debug utils, synchronization
validation and a caller-supplied structured Vulkan diagnostic callback.

All objects use one externally serialized CPU owner domain and one queue family.
Every access to the same native queue, even through another wrapper, must be
externally serialized. No internal mutex, worker or communication queue exists.
Tickets are queue-local admission evidence, NOT completion or global identity.
They cannot outlive their queue. `poll` advances only over signaled fences and
stops immediately on NOT_READY; `wait` uses Vulkan's bounded fence wait. Serial
exhaustion is capacity failure, never wraparound. Device Lost is returned and
latched on submission/fence failure; this component does not recover a device.

`CommandBatch` destruction cancels an unsubmitted recording; submission consumes
the recording on either outcome. A failed submission never advances the serial.
Staging slices are immutable borrows valid only for their originating recording.
Slot reuse happens after fence completion; cancelled recording uses a new epoch.
Staging capacity failure never allocates overflow storage. Partition size must
be a multiple of `nonCoherentAtomSize`, protecting neighboring in-flight reads
from rounded flush ranges. Read/write performs flush/invalidate and bounds checks;
CPU/GPU accesses to the same backing must not overlap.

Copy helpers operate on whole buffers or one full RGBA8 image. They retain V1's
pre-copy WAR/WAW and post-copy visibility barriers, conservatively scoped to all
commands on this queue. Overlapping uploads are serialized by each helper's
barriers. Image layout is supplied by the caller and left GENERAL. Readback adds
HOST_READ visibility; the caller obtains fence completion before invalidation
and CPU reading. Native command recording remains available through `native()`;
any custom recording must supply its own valid synchronization and lifetime.
No cross-queue ownership transfer, semaphore graph or native execution cache is
implemented. Ordinary dynamic data and submission count have no Simulation tie.

Shader binaries and raw Vulkan structures/handles must satisfy native Vulkan
valid-usage requirements and enabled features. Storage descriptor writes require
a set allocated by that pool and a compatible binding in its layout; no in-flight
descriptor mutation is allowed. This narrow mechanism does not retain/resolve
descriptor schemas or create a second resource registry. Native error results
preserve the signed VkResult bit pattern in `RenderError.args[0]`.

## GPU-safe retirement

The publishing owner first stops ALL new references, including references from
other views and recordings. It supplies the last successful submission using
the resource, then transfers the native owner into retirement. Capacity or owner
failure leaves that owner intact. `collect` destroys only entries whose serial
is fence-proven complete; admission may be out of order. Parent allocation and
device owners must remain alive until collection. When several submission
wrappers use one resource, the caller must first join the other uses; a ticket
from one queue is not proof about another queue's work.

Normal Buffer/Image/pipeline destruction does not wait for the device. Final
SubmissionQueue/StagingArena/RetirementQueue teardown joins admitted work by
fence; no public shutdown call is needed. A still-live CommandBatch is a CPU
borrow violation and invokes `std::terminate` even under NDEBUG. Unexpected
non-device-lost fence failures during a noexcept destructor are fatal. Lost-device
cleanup releases backing without falsifying successful completion. There is no
`vkDeviceWaitIdle`, per-resource blocking wait, lazy pipeline creation or zombie
semantic owner.

## Qualification and limits

Tests compile each public header independently, test forbidden includes, move/
destruction and native creation failures, bounded admission and serial retirement,
and execute a real compute shader with buffer/image readback. The fault library
compiles these same sources with private call-site instrumentation; production
and benchmarks have no seam. ASan instruments first-party native ownership code;
it does not instrument the Vulkan driver or validation layer DLLs.

The benchmark reports 100 samples after 20 transfer warmups: 1 MiB staging,
end/reset/submit, roundtrip including fence wait + readback, configuration-path
buffer create/destroy using a warmed VMA allocator, and retire/collect. It counts
first-party C++ new/new[] (including aligned forms), not VMA malloc or driver/DLL
heaps. Cold allocation is allowed. This is an R4 baseline, not a V1 speedup claim.

Public headers are synchronized to the three required install include prefixes;
the independent build consumes source public headers and generic dependencies,
not those Engine SDK copies. Complete installed SDK qualification remains deferred.
Products remain `EXPECTED_UNAVAILABLE` until R17. R5 requires separate approval.
