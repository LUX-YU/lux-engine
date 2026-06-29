#pragma once
#include <lux/cxx/concurrent/LockFreeQueue.hpp>
#include <lux/engine/render/resources/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/FrameServices.hpp>
#include <lux/engine/render/resources/lifecycle/ResourceCapacityMonitor.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/resources/memory/GPUBuffer.hpp>
#include <lux/engine/render/resources/memory/ArenaAllocator.hpp>
#include <lux/engine/render/resources/memory/ChainedArenaAllocator.hpp>
#include <lux/engine/render/core/VertexLayoutTypes.hpp>  // A-5: lightweight leaf (no gapi/vk/vk.hpp)
#include <lux/engine/render/resources/memory/StagingBuffer.hpp>

#include <lux/engine/render/core/Errors.hpp>
#include <lux/engine/math/AABB.hpp>
// MeshCpuRecord (bounds + BVH) is defined here — zero Vulkan dependencies.
#include <lux/engine/render/resources/mesh/MeshCpuData.hpp>
namespace lux::render { class TransferScheduler; }
#include <vector>
#include <array>
#include <span>
#include <cfloat>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <limits>
#include <type_traits>
#include <atomic>

namespace lux::render
{
    constexpr inline VertexLayoutId invalid_layout_id = kInvalidVertexLayoutId;

    enum class EIndexType : uint8_t { None = 0, UInt16, UInt32 };

    /// Max discrete LOD levels per mesh (LOD0 + up to 3 simplified). Caps the
    /// per-mesh LOD arrays here and the per-instance lod_mdc[] routing table.
    inline constexpr uint32_t kMaxMeshLod = 4;

    struct BufferRange {
        VkDeviceSize offset{ 0 };
        VkDeviceSize size{ 0 };
        bool isValid() const { return size > 0; }
    };

    // -------- GPU-Driven Segment Table Entry --------
    struct alignas(16) MeshInfoGpu {
        uint32_t index_first;    // Starting index in global IBO (index buffer)
        uint32_t index_count;    // Number of indices to draw in this segment
        int32_t  base_vertex;    // Starting vertex in global VBO (vertex buffer)

        // Data reserved for culling
        float    bounds_min[3];
        float    bounds_max[3];
    };
    static_assert(sizeof(MeshInfoGpu) % 16 == 0, "MeshInfoGpu must be 16B-aligned");

    // MeshCpuRecord is defined in MeshCpuData.hpp (included above).
    // It holds: local_bounds (AABB) + bvh (MeshBVH unique_ptr) + valid flag.
    // No Vulkan dependency — safe to use from game thread / headless code.

    /**
     * @brief GPU-side mesh data — available only after the render thread has
     *        completed flushPendingTransfers() and set ready = true.
     *
     * Render-thread private: only MeshRenderContributor, GPUDrivenContributor,
     * and flushPendingTransfers() access these fields.
     */
    struct MeshGpuRecord {
        VertexLayoutId      layout_id{0};
        uint32_t            vertex_stride{0};
        BufferRange         vertex_buffer_range{};
        BufferRange         index_buffer_range{};
        EIndexType          index_type{EIndexType::UInt32};
        uint32_t            index_count{0};
        // Discrete-LOD index sub-ranges within the (concatenated) IBO. lod_count>=1;
        // [0]=LOD0 (== the whole mesh when single-LOD). first_index is a GLOBAL index
        // (same units as MeshSectionRecord.first_index). Built in create/allocateOnly.
        uint8_t                            lod_count{1};
        std::array<uint32_t, kMaxMeshLod>  lod_index_first{};
        std::array<uint32_t, kMaxMeshLod>  lod_index_count{};
        uint16_t            vbo_segment{0};  ///< ChainedArena segment index for VBO
        uint16_t            ibo_segment{0};  ///< ChainedArena segment index for IBO
        VmaVirtualAllocation vbo_alloc_handle{VK_NULL_HANDLE}; ///< VMA virtual handle for VBO free
        VmaVirtualAllocation ibo_alloc_handle{VK_NULL_HANDLE}; ///< VMA virtual handle for IBO free
        SlotHandle          segment_slot{};  ///< Segment table entry for deferred ready update
        /// Written true once by the render thread (flushPendingTransfers).
        /// Read on the render thread only (MeshRenderContributor, GPUDrivenContributor).
        std::atomic<bool>   ready{false};

        MeshGpuRecord() = default;
        MeshGpuRecord(MeshGpuRecord&& o) noexcept
            : layout_id(o.layout_id), vertex_stride(o.vertex_stride),
              vertex_buffer_range(o.vertex_buffer_range),
              index_buffer_range(o.index_buffer_range),
              index_type(o.index_type), index_count(o.index_count),
              vbo_segment(o.vbo_segment), ibo_segment(o.ibo_segment),
              vbo_alloc_handle(o.vbo_alloc_handle), ibo_alloc_handle(o.ibo_alloc_handle),
              segment_slot(o.segment_slot),
              lod_count(o.lod_count),
              lod_index_first(o.lod_index_first), lod_index_count(o.lod_index_count),
              ready(o.ready.load(std::memory_order_relaxed)) {}
        MeshGpuRecord& operator=(MeshGpuRecord&& o) noexcept {
            if (this != &o) {
                layout_id           = o.layout_id;
                vertex_stride       = o.vertex_stride;
                vertex_buffer_range = o.vertex_buffer_range;
                index_buffer_range  = o.index_buffer_range;
                index_type          = o.index_type;
                index_count         = o.index_count;
                vbo_segment         = o.vbo_segment;
                ibo_segment         = o.ibo_segment;
                vbo_alloc_handle    = o.vbo_alloc_handle;
                ibo_alloc_handle    = o.ibo_alloc_handle;
                segment_slot        = o.segment_slot;
                lod_count           = o.lod_count;
                lod_index_first     = o.lod_index_first;
                lod_index_count     = o.lod_index_count;
                ready.store(o.ready.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
            }
            return *this;
        }
        MeshGpuRecord(const MeshGpuRecord&)                = delete;
        MeshGpuRecord& operator=(const MeshGpuRecord&)     = delete;
    };

    /// Pending CPU→GPU copy that will be flushed on the render thread.
    /// ONE staging buffer + BOTH the VBO and IBO copy regions for a single mesh.
    /// Carrying both in one queue entry (instead of two) makes the enqueue atomic:
    /// a full SPSC ring can no longer accept the VBO half while dropping the IBO
    /// half (which previously marked the mesh ready with a garbage index buffer).
    struct PendingTransfer {
        VkBuffer      stg_buf       { VK_NULL_HANDLE };
        VmaAllocation stg_alloc     { nullptr };
        // VBO copy region (vbo_size == 0 ⇒ none). Staging data at offset 0.
        VkBuffer      vbo_dst_buf   { VK_NULL_HANDLE };
        VkDeviceSize  vbo_dst_offset{ 0 };
        VkDeviceSize  vbo_size      { 0 };
        // IBO copy region (ibo_size == 0 ⇒ none). Staging data follows the VBO at
        // offset vbo_size.
        VkBuffer      ibo_dst_buf   { VK_NULL_HANDLE };
        VkDeviceSize  ibo_dst_offset{ 0 };
        VkDeviceSize  ibo_size      { 0 };
        uint32_t      mesh_index    { 0 };
    };

    struct MeshCreateInfo {
        VertexLayoutId              layout_id{ 0 };
        uint32_t                    vertex_stride{ 0 };
        std::span<const std::byte>  vertex_buffer{};
        std::span<const std::byte>  index_buffer{};   // concatenated [LOD0 .. LODn]
        EIndexType                  index_type{ EIndexType::None };
        std::optional<math::AABB>   bounds{};
        // Per-LOD index counts within `index_buffer` (LOD0 first; sum == total).
        // Empty ⇒ single-LOD mesh (the whole index_buffer is LOD0).
        std::span<const uint32_t>   lod_index_counts{};
    };

    /// Result of allocateOnly(): handle + arena ranges for the worker thread.
    struct MeshAllocResult {
        MeshHandle  handle{};
        BufferRange vbo_range{};
        BufferRange ibo_range{};
        uint16_t    vbo_segment{0};
        uint16_t    ibo_segment{0};
    };

    class LUX_FUNCTION_PUBLIC MeshResources final
        : public GPUResourceBase<MeshResources, EGPUResourceType::Mesh>
        , public IGlobalFrameService
    {
    public:

        struct InitInfo
        {
            DeviceContext* device;
            VkDeviceSize                   vertex_arena_bytes{ 64ull * 1024 * 1024 };
            VkDeviceSize                   index_arena_bytes{ 32ull * 1024 * 1024 };
            bool                           enable_device_address{ true };
            /// Must match the swap-chain frames-in-flight count so deferred
            /// staging-buffer deletion uses the correct ring slot.
            uint32_t                       frames_in_flight{ 2 };
            /// Maximum number of meshes that can ever be created during the
            /// lifetime of this MeshResources.  cpu_records_ and gpu_records_
            /// are reserved to this capacity in init() so that subsequent
            /// create() calls on the game thread NEVER trigger a reallocation,
            /// which would invalidate any indices the render thread holds.
            uint32_t                       mesh_max_count{ 65536 };

            // Segment table SSBO initial config (static 1 slice)
            SSBOInitConfig                 segments_ssbo_cfg{
                nullptr,  /*device_context*/
                nullptr,  /*deferred_queue*/
                16384,    /*initial_dense_capacity*/
                1,        /*slices*/
                false,    /*allow_shader_write*/
                false     /*clear_on_remove*/
            };
        };

        // ---------- Lifecycle ----------
        ~MeshResources();

        [[nodiscard]] bool init(const InitInfo& ci);

        void shutdown();

        // ---------- Render-thread upload flush ----------

        /**
         * @brief  Record pending CPU→GPU copies and insert a pipeline barrier.
         *
         * Must be called from the render thread at the start of onRecord(),
         * before processFrame().  Drains the SPSC pending-transfer queue,
         * records vkCmdCopyBuffer commands into @p cmd, inserts a
         * TRANSFER_WRITE → VERTEX_INPUT memory barrier, marks each mesh as
         * ready, and schedules the staging buffers for deferred deletion.
         *
         * @param cmd  Command buffer currently being recorded (render thread).
         * @param fi   Frame-in-flight index (same as passed to beginFrame).
         */
        void flushPendingTransfers(VkCommandBuffer cmd, uint32_t fi);

        /**
         * @brief  Destroy staging buffers whose GPU copies are complete.
         *
         * Must be called from the render thread inside beginFrame(), after the
         * frame fence for @p fi has been waited on.
         *
         * @param fi  Frame-in-flight index whose staging ring slot to retire.
         */
        void retireFrameStagingBuffers(uint32_t fi);

        // ---------- Async acquire barriers (render thread) ----------

        /// Queue a buffer acquire barrier for QFOT (called during completion drain).
        void pushAcquireBarrier(VkBuffer buf, VkDeviceSize offset, VkDeviceSize size,
                                uint32_t src_queue_family, uint32_t dst_queue_family);

        /// Record all pending acquire barriers into @p cmd, then clear the list.
        void recordAcquireBarriers(VkCommandBuffer cmd);

        void onBeginFrame(const FrameStamp& stamp) override
        {
            current_fi_ = stamp.slotIndex();
            retireFrameStagingBuffers(current_fi_);
        }

        // ── Transfer scheduler integration ──

        void submitTransfers(TransferScheduler& scheduler);

        // ---------- StagingOnly fallback (iGPU, render thread) ----------

        /// Staging copy to be recorded by the render thread (StagingOnly mode).
        struct PendingStagingCopy {
            VkBuffer     stg_buf;
            VkBuffer     vbo_dst;
            VkDeviceSize vbo_stg_offset;
            VkDeviceSize vbo_dst_offset;
            VkDeviceSize vbo_size;
            VkBuffer     ibo_dst;        ///< VK_NULL_HANDLE if no IBO
            VkDeviceSize ibo_stg_offset;
            VkDeviceSize ibo_dst_offset;
            VkDeviceSize ibo_size;
            uint32_t     mesh_index;
            uint16_t      vbo_segment;    ///< VBO buffer segment index
            uint16_t      ibo_segment;    ///< IBO buffer segment index
        };

        /// Queue a staging copy (called in tick() when timeline_value == 0).
        void pushStagingCopy(const PendingStagingCopy& copy);

        /// Record all pending staging copies + barrier, then markReady each mesh.
        void recordStagingCopies(VkCommandBuffer cmd);

        // ---------- Byte stream/Strongly typed (for aggregation/packing, e.g. ModelImporter) ----------
        Expected<MeshHandle> create(const MeshCreateInfo& ci);

        /**
         * @brief Allocate VBO/IBO arena ranges + slot without staging transfer.
         *
         * Used by the async upload path: the handler calls allocateOnly() on
         * the render thread (Phase 1), then the worker thread copies data
         * directly into the arena at the returned offsets.
         * The mesh starts with ready == false; call markReady() after the
         * GPU copy is complete.
         */
        Expected<MeshAllocResult> allocateOnly(const MeshCreateInfo& ci);

        /**
         * @brief Flip the ready flag for a mesh after its async GPU copy completes.
         *
         * Must be called from the render thread after the transfer queue's
         * timeline semaphore has been waited on (or after the graphics queue
         * records the copy in the StagingOnly fallback path).
         */
        void markReady(uint32_t mesh_index);

        /// Check whether a mesh's GPU data is ready for rendering.
        [[nodiscard]] bool isReady(uint32_t mesh_index) const noexcept
        {
            return mesh_index < gpu_records_.size()
                && gpu_records_[mesh_index].ready.load(std::memory_order_acquire);
        }

        template<typename TVertex>
        Expected<MeshHandle> create(
            VertexLayoutId layout_id,
            std::span<const TVertex>   vertices,
            std::span<const uint32_t>  indices,
            std::optional<math::AABB>  bounds = std::nullopt
        )
        {
            MeshCreateInfo ci{};
            ci.layout_id = layout_id;
            ci.vertex_stride = sizeof(TVertex);
            ci.vertex_buffer = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(vertices.data()),
                vertices.size() * sizeof(TVertex)
            );
            ci.index_buffer = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(indices.data()),
                indices.size() * sizeof(uint32_t)
            );
            ci.index_type = EIndexType::UInt32;

            // Auto-compute AABB from vertex positions when none provided.
            if (!bounds.has_value() && !vertices.empty()) {
                math::AABB auto_bounds;
                for (const auto& v : vertices)
                    auto_bounds.merge(Eigen::Vector3f(v.position));
                ci.bounds = auto_bounds;
            } else {
                ci.bounds = bounds;
            }

            return create(ci); // Calls the byte-stream version below
        }

        // ---------- Destroy / Query ----------
        bool            destroy(MeshHandle h);
        bool            alive(MeshHandle h) const;

        Expected<VertexLayoutId> layoutId(MeshHandle h) const;

        // ---------- CPU record access (any thread, game-thread primary) ----------

        /**
         * @brief Local-space bounds + BVH — available as soon as the mesh slot is live.
         * Safe to call from the game thread; no Vulkan dependency.
         */
        [[nodiscard]] const MeshCpuRecord* getCpuRecord(MeshHandle h) const noexcept
        {
            return alive(h) ? &cpu_records_[h.index] : nullptr;
        }

        /**
         * @brief GPU-side layout/buffer metadata — valid only when ready == true.
         * Render-thread only.
         */
        [[nodiscard]] const MeshGpuRecord* getGpuRecord(MeshHandle h) const noexcept
        {
            return alive(h) ? &gpu_records_[h.index] : nullptr;
        }

        /// Get the per-mesh BVH (for precise picking).  Returns nullptr if not built.
        [[nodiscard]] const math::MeshBVH* getMeshBVH(MeshHandle h) const noexcept
        {
            if (!alive(h)) return nullptr;
            return cpu_records_[h.index].bvh.get();
        }

        // VBO/IBO — segment-aware accessors
        /// Return the VkBuffer for a specific VBO segment (0 = original buffer).
        VkBuffer        vertexBuffer(uint16_t segment = 0) const { return segment < vbo_buffers_.size() ? vbo_buffers_[segment] : VK_NULL_HANDLE; }
        /// Return the VkBuffer for a specific IBO segment (0 = original buffer).
        VkBuffer        indexBuffer(uint16_t segment = 0)  const { return segment < ibo_buffers_.size() ? ibo_buffers_[segment] : VK_NULL_HANDLE; }
        /// Convenience: return VBO for a given mesh handle.
        VkBuffer        vertexBufferForMesh(MeshHandle h) const { return alive(h) ? vertexBuffer(gpu_records_[h.index].vbo_segment) : VK_NULL_HANDLE; }
        /// Convenience: return IBO for a given mesh handle.
        VkBuffer        indexBufferForMesh(MeshHandle h) const  { return alive(h) ? indexBuffer(gpu_records_[h.index].ibo_segment) : VK_NULL_HANDLE; }
        /// Number of VBO segments currently allocated.
        uint16_t        vboSegmentCount() const { return static_cast<uint16_t>(vbo_buffers_.size()); }
        /// Number of IBO segments currently allocated.
        uint16_t        iboSegmentCount() const { return static_cast<uint16_t>(ibo_buffers_.size()); }
        BufferRange     vertexRange(MeshHandle h) const { return alive(h) ? gpu_records_[h.index].vertex_buffer_range : BufferRange{}; }
        BufferRange     indexRange(MeshHandle h)  const { return alive(h) ? gpu_records_[h.index].index_buffer_range : BufferRange{}; }
        uint32_t        vertexStride(MeshHandle h)const { return alive(h) ? gpu_records_[h.index].vertex_stride : 0u; }
		uint32_t        indexCount(MeshHandle h)  const { return alive(h) ? gpu_records_[h.index].index_count : 0u; }
        EIndexType      indexType(MeshHandle h)   const { return alive(h) ? gpu_records_[h.index].index_type : EIndexType::None; }

        // ---------- Arena access (upload thread) ----------

        /// Allocation result including segment index.
        struct SegmentedRange {
            BufferRange range{};
            uint16_t    segment{0};
            VmaVirtualAllocation alloc_handle{VK_NULL_HANDLE};
        };

        /**
         * @brief Allocate a region from the VBO chained arena.
         *
         * Thread-safety: must be called from a single producer thread
         * (the resource upload thread).  The ChainedArenaAllocator is not
         * currently thread-safe for concurrent allocations.
         *
         * @param bytes      Number of bytes to allocate.
         * @param alignment  Alignment in bytes (default 256).
         * @return Allocated range with segment index, or an error on OOM.
         */
        Expected<SegmentedRange> allocateVBORange(VkDeviceSize bytes, VkDeviceSize alignment = 256)
        {
            auto alloc = vbo_arena_.allocate(bytes, alignment);
            if (!alloc.valid())
                return lux::cxx::unexpected(make_error_code(ERenderError::OutOfMemory));
            return SegmentedRange{ BufferRange{ alloc.offset, alloc.size }, alloc.segment_index, alloc.handle };
        }

        /**
         * @brief Allocate a region from the IBO chained arena.
         * @see allocateVBORange for thread-safety notes.
         */
        Expected<SegmentedRange> allocateIBORange(VkDeviceSize bytes, VkDeviceSize alignment = 256)
        {
            auto alloc = ibo_arena_.allocate(bytes, alignment);
            if (!alloc.valid())
                return lux::cxx::unexpected(make_error_code(ERenderError::OutOfMemory));
            return SegmentedRange{ BufferRange{ alloc.offset, alloc.size }, alloc.segment_index, alloc.handle };
        }

        /**
         * @brief Return a VBO region to the chained arena for reuse.
         * Thread-safety: same as allocateVBORange.
         */
        void freeVBORange(const BufferRange& range, uint16_t segment,
                          VmaVirtualAllocation handle)
        {
            if (range.isValid())
                vbo_arena_.free({ segment, range.offset, range.size, handle });
        }

        /**
         * @brief Return an IBO region to the chained arena for reuse.
         * Thread-safety: same as allocateVBORange.
         */
        void freeIBORange(const BufferRange& range, uint16_t segment, VmaVirtualAllocation handle)
        {
            if (range.isValid())
                ibo_arena_.free({ segment, range.offset, range.size, handle });
        }

        // Segment table SSBO
        VkBuffer        segmentsBuffer() const { return segments_ssbo_.dataBuffer(); }
        uint32_t        segmentsBaseForSlice(uint32_t slice) const { return segments_ssbo_.baseIndexForSlice(slice); }
        void            writeSegmentsDescriptor(VkDescriptorSet set, uint32_t binding) const { segments_ssbo_.writeDataDescriptor(set, binding); }

        // ========== IGPUResource Interface Implementation ==========

        bool isInitialized() const { return initialized_; }

        std::string getDebugInfo() const
        {
            return "MeshResources: Active (VBO/IBO + Segments SSBO)";
        }

        /// VBO capacity snapshot for monitoring.
        [[nodiscard]] CapacityStatus vboCapacityStatus() const noexcept
        {
            return { vbo_arena_.totalUsedBytes(), vbo_arena_.totalCapacity(), vbo_arena_.totalCapacity() };
        }

        /// IBO capacity snapshot for monitoring.
        [[nodiscard]] CapacityStatus iboCapacityStatus() const noexcept
        {
            return { ibo_arena_.totalUsedBytes(), ibo_arena_.totalCapacity(), ibo_arena_.totalCapacity() };
        }

        // ---------- Defragmentation (render thread) ----------

        /**
         * @brief  Run arena defragmentation if fragmentation exceeds threshold.
         *
         * Render-thread only.  For each VBO/IBO segment whose
         * fragmentationRatio() > @p threshold, records vkCmdCopyBuffer
         * commands within the same GPU buffer to compact live allocations,
         * then updates gpu_records_ and the segment SSBO accordingly.
         *
         * @param cmd        Command buffer currently being recorded.
         * @param threshold  Fragmentation ratio in [0,1] above which defrag fires.
         *                   Default 0.3 (30%).
         * @return Number of copy operations recorded.
         */
        uint32_t maybeDefragment(VkCommandBuffer cmd, float threshold = 0.3f);

        /// Late-bind centralized deferred destroy queue to segments SSBO.
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            segments_ssbo_.setDeferredQueue(q);
        }

    private:
        // —— Tools —— //
        static VkDeviceSize align256(VkDeviceSize x) { return (x + 255) & ~255ull; }

        // DESIGN-04: ChainedArenaAllocator-based sub-allocation with free+coalescing
        Expected<SegmentedRange>
        suballoc(ChainedArenaAllocator& arena, std::span<const std::byte> data, uint64_t alignment = 256);

        /// Allocate a new GPU buffer segment and add it to the vector / chained arena.
        bool addBufferSegment(VkDeviceSize bytes, VkBufferUsageFlags usage,
                              std::vector<VkBuffer>& bufs, std::vector<VmaAllocation>& allocs,
                              ChainedArenaAllocator& arena);

        /// Legacy helper kept for init() — creates the first segment.
        bool createArena(VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer& out, VmaAllocation& out_alloc);

        // Game-thread helper: allocate a staging buffer, memcpy, push to SPSC queue.
        void enqueueStagingTransfer(std::span<const std::byte> vbo_data, VkDeviceSize vbo_offset,
                                    uint16_t vbo_segment,
                                    std::span<const std::byte> ibo_data, VkDeviceSize ibo_offset,
                                    uint16_t ibo_segment,
                                    uint32_t mesh_index);

        static uint32_t calcIndexStartGlobal(EIndexType it, const BufferRange& ir, uint32_t sub_first)
        {
            const uint32_t index_size = (it == EIndexType::UInt16 ? 2u : 4u);
            return static_cast<uint32_t>(ir.offset / index_size) + sub_first;
        }

    private:
        DeviceContext*                       device_ctx_{ nullptr };

        // Per-segment VBO/IBO GPU buffers (index 0 = initial segment)
        std::vector<VkBuffer>                vbo_buffers_;
        std::vector<VmaAllocation>           vbo_allocs_;
        std::vector<VkBuffer>                ibo_buffers_;
        std::vector<VmaAllocation>           ibo_allocs_;

        VkDeviceSize                         vbo_segment_cap_{ 0 };  ///< Capacity of each new VBO segment
        VkDeviceSize                         ibo_segment_cap_{ 0 };  ///< Capacity of each new IBO segment
        VkBufferUsageFlags                   vbo_usage_flags_{ 0 };  ///< Usage flags for VBO buffer creation
        VkBufferUsageFlags                   ibo_usage_flags_{ 0 };  ///< Usage flags for IBO buffer creation
        // DESIGN-04: Free-list chained arena allocators replace single arenas
        ChainedArenaAllocator                vbo_arena_;
        ChainedArenaAllocator                ibo_arena_;

        // Segment table SSBO (geometry segment info; material related fields filled by upper layer later or use default on GPU side)
        SlicedSSBO<MeshInfoGpu>              segments_ssbo_;

        std::vector<MeshCpuRecord>           cpu_records_;  ///< Game-thread readable (bounds, BVH)
        std::vector<MeshGpuRecord>           gpu_records_;  ///< Render-thread private (VkBuffer ranges)
        std::vector<uint32_t>                gens_;
        /// Number of slots ever allocated (always <= cpu_records_.size()).
        /// Written by the game thread with release after fully populating
        /// cpu_records_[id] and gpu_records_[id], so the render thread cannot
        /// observe a half-constructed record.
        std::atomic<uint32_t>                slot_count_{0};
        std::vector<uint32_t>                free_;
        /// Hard cap (= CreateInfo.mesh_max_count, default 65536). cpu_records_/
        /// gpu_records_/gens_ are reserve()'d to this in init() so push_back never
        /// reallocates storage the game thread reads concurrently. create()/
        /// allocateOnly() ENFORCE it (reject growth past it) — otherwise the cap
        /// is a comment, not a guarantee, and the reserve()'s anti-UAF intent is
        /// defeated the moment a scene exceeds it. (C-4)
        uint32_t                             mesh_max_count_ = 0;

        // Async upload infrastructure (Phase A)
        lux::cxx::SpscLockFreeRingQueue<PendingTransfer>  pending_queue_{ 256 };

        /// Ring of per-frame staging-buffer deletion lists.
        /// Slot fi is freed in retireFrameStagingBuffers(fi) after the fence wait.
        std::vector<std::vector<StagingBuffer>>           deferred_deletions_;

        /// A VBO/IBO arena range awaiting frames-in-flight retirement. destroy()
        /// must NOT return the range to the arena immediately: a destroy-then-
        /// create within FIF frames could re-allocate the identical bytes and the
        /// async transfer-queue write would corrupt vertices that in-flight
        /// graphics frames are still drawing. Freed in retireFrameStagingBuffers()
        /// once the slot's fence has been waited, mirroring deferred_deletions_.
        struct RetiredArenaRange {
            bool                 is_vbo{true};
            uint16_t             segment{0};
            BufferRange          range{};
            VmaVirtualAllocation handle{nullptr};
        };
        std::vector<std::vector<RetiredArenaRange>>       retired_ranges_;

        /// Pending acquire barriers for QFOT (render-thread only, drained per frame).
        std::vector<VkBufferMemoryBarrier2>               pending_acquire_barriers_;

        /// Pending staging copies for StagingOnly fallback (render-thread only).
        std::vector<PendingStagingCopy>                   pending_staging_copies_;

        uint32_t current_fi_{0};
    };

} // namespace lux::render
