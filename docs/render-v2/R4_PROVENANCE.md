# R4 native mechanism provenance

Authority: user R4 authorization, accepted R3 `f9b943c9e768ef2c9d408962f0d5a32d2cf3de74`.
All V1 paths below are under `render_legacy/modules/function/render/vulkan/`,
whose original blobs are pinned to `a669409a289a6fa4092f21176397795b1cdb7f3e`.
No frozen source or prior verdict is modified.

| V1 reference | Preserved mechanism/invariant | R4 scope/difference and evidence |
| --- | --- | --- |
| `src/gpu/VulkanContext.cpp`, `VmaImplementation.cpp` | Enumerate/check capabilities; device before allocator; allocator before allocations | Three independent complete RAII owners, Vulkan 1.3 + sync2 only; no context hierarchy, presentation or feature inventory. Real device test and factory failure hooks. |
| `src/gpu/memory/VmaTypes.cpp` | Single VMA allocation owner, moved-from handle cleared, destroy exactly once | Explicit host/device access; checked flush/invalidate; Buffer/Image error candidates clean up. Move/assignment/live-count tests, real image and buffer readback. |
| `src/gpu/descriptor/**`, `src/gpu/pipeline/**`; ownership tests | Native create/destroy matching, pool owns sets, candidate failures preserve active objects | Minimal layouts/pool/storage write and compute pipeline; no giant PipelineManager or feature layouts. Fault tests cover native creation, failed allocation and partial pipeline output. |
| `src/gpu/utils/StagingRingBuffer.cpp` | Fixed partition, power-of-two alignment, partition reset only after GPU completion | Partition indexed by admitted submission rather than frame slot; recording epoch handles cancelled candidates. Non-coherent atom isolation + checked flush extends the original coherent mapping precondition. No overflow allocation fallback. Capacity/flush failure and three-slot tests. |
| `src/gpu/transfer/TransferScheduler.cpp` | Pre-copy READ/WRITE → TRANSFER_WRITE, post-copy visibility; overlapping write serialization; explicit image transitions | Individual native copy helpers retain those ordering predicates with conservative whole-resource barriers. No contributor/scheduler hierarchy, batch coalescing, feature payload or async worker. One family, full RGBA8 image; real sync-validation compute and repeated image/buffer roundtrips exercise WAR/WAW/RAW. |
| `src/gpu/lifecycle/DeferredDestroyQueue.cpp` | last-use serial ≤ completed serial before native destruction; final teardown must prove completion | Completion is now queue-scoped fence evidence, independent of frames. Preallocated optional native owners replace dynamic free-list/FIFO storage. A bounded scan supports retirement arriving out of serial order; it does not alter the completion predicate. Three in-flight slots, out-of-order retirement, full-capacity retained candidate, repeated collection and final RAII join tests. |

The fixed native submission ring owns command buffers and fences. Successful
queue submission advances its serial; a slot cannot reset while its previous
fence is incomplete. No Simulation or Frame authority is introduced. Old
RenderContext/DeviceContext/ResourceContext, TransferScheduler ownership and
GeneralRenderServer are not copied. GPU shader/math/rendering algorithms are not
rewritten; R4 adds a tiny test compute shader only.

These are scoped mechanism migrations, not a claim that V1's entire asynchronous
transfer workload has been reproduced. There is no equivalent V1 benchmark with
this isolated workload and configuration, so R4 records an initial baseline;
percentage speedup/regression is NOT_APPLICABLE, not fabricated. Broader feature
transfer scheduling, multi-queue operation and graphics pipelines await real
consumers. Existing `render.transfer_idle` and cross-frame skinning WAR remain
known historical limitations, NOT_RUN here, never reclassified by this smoke.

Native correctness references used to cross-check the implementation:

- [Vulkan synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html): execution and memory dependencies, fence completion.
- [Vulkan synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html): transfer/compute/readback barriers.
- [Queue submission](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html): native admission, fence and externally synchronized queue access.
- [VMA mapping](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/memory_mapping.html): non-coherent flush/invalidate and allocation-relative mapping.

R5 obligations remain independent progress/pacing, real terminal Runtime error
and async diagnostics, ticket consume/abandon closure, actual target requirements,
and complete native graph/cache keys. R5/R6 must prove one Scene with multiple
Views, differing graph/pass compositions, shared persistent state and independent
target/history lifetime. None of those production structures are implemented in R4.
