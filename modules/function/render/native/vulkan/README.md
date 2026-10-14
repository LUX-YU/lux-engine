# Native Vulkan ownership

`InstanceContext` is owned by bootstrap, outside `DeviceSession`. A session borrows the instance and
owns one device, VMA allocator, unique queue owners and ticket-driven retirement. Aliased queue roles
resolve to the same owner. No Graph, Scene or IPC object is introduced by this foundation.

Resource factories return move-only Buffer/Image owners. Their allocator and device must outlive them.
Code/context owners will be composed under the session lifetime; an outstanding ticket is queue-scoped
evidence of actual accepted work, never a frame index. Failure of a subsequent submit cannot erase
`lastSubmitted()`. Retirement transfers ownership only on success and destroys it only after completion.
Final teardown drains all queue owners before allocator/device release; no individual resource waits idle.

## Algorithm provenance

The device selection, VMA Buffer/Image RAII, bounded command/fence pool, synchronization2 submission,
queue-scoped ticket and retirement algorithms are migrated from commit
`960e755395be2c9f78a86339a062c507826c4eb4`, `modules/function/render/vulkan/`:

- `device/Device`, `device/Vma`, `memory/Memory`: actual resource factories and reverse RAII cleanup.
- `transfer/Submission`: accepted-submission serial, fence completion, cross-queue timeline waits.
- `retirement/Retirement`: fixed-capacity owner transfer and ticket-gated destruction.
- `pinclude/Native.hpp`: test-only compile-time fault hook; no production indirect call.

The new path is `render/native/vulkan`; the old modules are not linked, and Legacy is read-only.
The old `DeviceCaps` bridge was removed, `VulkanInstance` became `InstanceContext`, and the retirement
variant includes only Buffer/Image and an already-owned composite. Descriptor/Graph implementations
are not restored. `DeviceSession` consolidates real aliased queues instead of adding another queue system.

BOUNDARY checks cover cold factory arguments and external ticket/owner inputs. STATE-CHANGE checks
cover all native results and fence completion. Successful construction establishes the internal INVARIANT
of stable device/allocator/queue borrows; no per-frame capability discovery is required.

The foundation test executes real GPU buffer fill/readback and cross-queue submission under Validation
and SyncValidation. The instrumented build exercises native initialization failures, partial admission,
retirement delayed completion and exact owner destruction. Later graph execution has its own qualification;
these foundation tests do not imply a compiled Graph or product PASS.
