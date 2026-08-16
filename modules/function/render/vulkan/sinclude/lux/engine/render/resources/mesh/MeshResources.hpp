#pragma once
#include <lux/cxx/concurrent/LockFreeQueue.hpp>
#include <lux/engine/render/gpu/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp>
#include <lux/engine/render/gpu/memory/ArenaAllocator.hpp>
#include <lux/engine/render/gpu/memory/ChainedArenaAllocator.hpp>
#include <lux/engine/function/render/client/core/VertexLayoutTypes.hpp>  // A-5: lightweight leaf (no gapi/vk/vk.hpp)
#include <lux/engine/render/gpu/memory/StagingBuffer.hpp>

#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/math/AABB.hpp>
// MeshCpuRecord (bounds + BVH) is defined here — zero Vulkan dependencies.
#include <lux/engine/render/resources/mesh/MeshCpuData.hpp>
#include <lux/engine/render/resources/mesh/StableRecordPages.hpp>
#include <lux/engine/resource/deployment/RuntimeCapacity.hpp>
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
#include <utility>

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
     *        completed the upload and called markReady().
     *
     * Render-thread private: only MeshRenderContributor, GPUDrivenContributor,
     * and markReady() access these fields.
     */
    struct MeshGpuRecord {
        VertexLayoutId      layout_id{0};
        uint32_t            vertex_stride{0};
        BufferRange         vertex_buffer_range{};
        BufferRange         index_buffer_range{};
        EIndexType          index_type{EIndexType::UInt32};
        uint32_t            index_count{0};
        // Discrete-LOD index sub-ranges within the (concatenated) IBO. lod_count>=1;
        // [0]=LOD0 (== the whole mesh when single-LOD). Built in create/allocateOnly.
        // ⚠ lod_index_first 是 **ibo_segment 段内** 的索引下标(同 MeshSectionRecord
        //   .first_index 的单位),不是跨段全局下标 —— 只有配上 ibo_segment 才能定位
        //   到具体缓冲。见 calcIndexStartInSegment。
        uint8_t                            lod_count{1};
        std::array<uint32_t, kMaxMeshLod>  lod_index_first{};
        std::array<uint32_t, kMaxMeshLod>  lod_index_count{};
        uint16_t            vbo_segment{0};  ///< ChainedArena segment index for VBO
        uint16_t            ibo_segment{0};  ///< ChainedArena segment index for IBO
        VmaVirtualAllocation vbo_alloc_handle{VK_NULL_HANDLE}; ///< VMA virtual handle for VBO free
        VmaVirtualAllocation ibo_alloc_handle{VK_NULL_HANDLE}; ///< VMA virtual handle for IBO free
        SlotHandle          segment_slot{};  ///< Segment table entry for deferred ready update
        /// Written true once by markReady(), read by the render contributors.
        /// A plain bool: everything that touches a MeshGpuRecord runs on the
        /// server/render thread. (It used to be std::atomic<bool>, which forced
        /// the hand-written move ops below to exist at all — the upload worker
        /// threads never see this struct; they get a raw VkBuffer + offset and
        /// hand completions back for the render thread to apply.)
        bool                ready{false};

        MeshGpuRecord()                                    = default;
        MeshGpuRecord(MeshGpuRecord&&) noexcept            = default;
        MeshGpuRecord& operator=(MeshGpuRecord&&) noexcept = default;
        MeshGpuRecord(const MeshGpuRecord&)                = delete;
        MeshGpuRecord& operator=(const MeshGpuRecord&)     = delete;
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
    {
    public:
        struct ArenaTelemetry final
        {
            std::uint32_t segment_count{0u};
            std::uint32_t growth_count{0u};
            std::uint64_t used_bytes{0u};
            std::uint64_t free_bytes{0u};
            std::uint64_t largest_free_block{0u};
            float fragmentation{0.0f};
        };

        struct InitInfo
        {
            DeviceContext* device;
            VkDeviceSize                   vertex_arena_bytes{ 64ull * 1024 * 1024 };
            VkDeviceSize                   index_arena_bytes{ 32ull * 1024 * 1024 };
            bool                           enable_device_address{ true };
            /// Must match the swap-chain frames-in-flight count so deferred
            /// staging-buffer deletion uses the correct ring slot.
            uint32_t                       frames_in_flight{ 2 };
            /// CapacityPlan admission ceiling. Record address stability is
            /// provided independently by 4096-record pages.
            uint32_t                       mesh_max_count{ 65536 };
            /// Combined VBO+IBO committed-capacity admission ceiling.
            std::uint64_t                  geometry_capacity_bytes{
                512ull * 1024u * 1024u};

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

        // ---------- Render-thread FIF retirement ----------

        /**
         * @brief  Return arena ranges + segment-table slots whose frame is done.
         *
         * Must be called from the render thread inside beginFrame(), after the
         * frame fence for @p fi has been waited on. (The name still says
         * "StagingBuffers" for call-site continuity; the staging ring itself
         * went away with the synchronous upload path — the server owns staging
         * buffers now.)
         *
         * @param fi  Frame-in-flight index whose retirement slot to drain.
         */
        void retireFrameStagingBuffers(uint32_t fi);

        // ---------- Async acquire barriers (render thread) ----------

        /// Queue a buffer acquire barrier for QFOT (called during completion drain).
        void pushAcquireBarrier(VkBuffer buf, VkDeviceSize offset, VkDeviceSize size,
                                uint32_t src_queue_family, uint32_t dst_queue_family);

        /// Record all pending acquire barriers into @p cmd, then clear the list.
        void recordAcquireBarriers(VkCommandBuffer cmd);

        void onFrameBeginMaintenance(const FrameStamp& stamp)
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
        /// 现役上传走 GpuTransferPipeline；这条是同步录制的备用路径，保留给
        /// 不创建 transfer 线程的嵌入/工具进程。
        void recordStagingCopies(VkCommandBuffer cmd);

        /**
         * @brief Allocate VBO/IBO arena ranges + slot without staging transfer.
         *
         * Used by the async upload path: the handler calls allocateOnly() on
         * the render thread, then the worker thread copies data
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
                && gpu_records_[mesh_index].ready;
        }

        // ---------- Destroy / Query ----------
        /// Render-instance ownership. A destroy request becomes pending while
        /// one or more live/retiring instances retain the handle; the final
        /// release performs the ordinary fence-deferred arena retirement.
        [[nodiscard]] bool retainForInstance(MeshHandle h) noexcept;
        void releaseFromInstance(MeshHandle h) noexcept;
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
        [[nodiscard]] std::uint64_t iboTopologySerial() const noexcept
        {
            return ibo_topology_serial_;
        }
        BufferRange     vertexRange(MeshHandle h) const { return alive(h) ? gpu_records_[h.index].vertex_buffer_range : BufferRange{}; }
        BufferRange     indexRange(MeshHandle h)  const { return alive(h) ? gpu_records_[h.index].index_buffer_range : BufferRange{}; }
        uint32_t        vertexStride(MeshHandle h)const { return alive(h) ? gpu_records_[h.index].vertex_stride : 0u; }
		uint32_t        indexCount(MeshHandle h)  const { return alive(h) ? gpu_records_[h.index].index_count : 0u; }
        EIndexType      indexType(MeshHandle h)   const { return alive(h) ? gpu_records_[h.index].index_type : EIndexType::None; }
        [[nodiscard]] const std::optional<lux::deployment::CapacityShortfall>&
        lastCapacityShortfall() const noexcept
        {
            return last_capacity_shortfall_;
        }

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
                return renderFailure<err::memory::OutOfMemory>();
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
                return renderFailure<err::memory::OutOfMemory>();
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
        VkBuffer        segmentsBuffer() const { return segments_ssbo_.buffer(); }
        uint32_t        segmentsBaseForSlice(uint32_t slice) const { return segments_ssbo_.baseIndexForSlice(slice); }
        void            writeSegmentsDescriptor(VkDescriptorSet set, uint32_t binding) const { segments_ssbo_.writeDescriptor(set, binding); }

        // ========== IGPUResource Interface Implementation ==========

        bool isInitialized() const { return initialized_; }

        [[nodiscard]] ArenaTelemetry vboTelemetry() const noexcept
        {
            return arenaTelemetry(vbo_arena_);
        }

        [[nodiscard]] ArenaTelemetry iboTelemetry() const noexcept
        {
            return arenaTelemetry(ibo_arena_);
        }

        /// Late-bind centralized deferred destroy queue to segments SSBO.
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            segments_ssbo_.setDeferredQueue(q);
        }

    private:
        [[nodiscard]] static ArenaTelemetry arenaTelemetry(
            const ChainedArenaAllocator& arena) noexcept
        {
            const auto capacity = arena.totalCapacity();
            const auto used = arena.totalUsedBytes();
            const auto segments = arena.segmentCount();
            return ArenaTelemetry{
                segments,
                segments == 0u ? 0u : segments - 1u,
                used,
                capacity > used ? capacity - used : 0u,
                arena.largestFreeBlock(),
                arena.fragmentationRatio()};
        }

        // —— Tools —— //
        static VkDeviceSize align256(VkDeviceSize x) { return (x + 255) & ~255ull; }

        // DESIGN-04: ChainedArenaAllocator-based sub-allocation with free+coalescing
        Expected<SegmentedRange>
        suballoc(ChainedArenaAllocator& arena, std::span<const std::byte> data, uint64_t alignment = 256);

        /// Allocate a new GPU buffer segment and add it to the vector / chained arena.
        bool addBufferSegment(VkDeviceSize bytes, VkBufferUsageFlags usage,
                              std::vector<VkBuffer>& bufs, std::vector<VmaAllocation>& allocs,
                              ChainedArenaAllocator& arena);
        void rollbackUnpublishedSegments(
            std::uint16_t vbo_segment_count,
            std::uint16_t ibo_segment_count) noexcept;

        /// Legacy helper kept for init() — creates the first segment.
        bool createArena(VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer& out, VmaAllocation& out_alloc);

        /// create() 与 allocateOnly() 的公共前段:校验 → index_type 推断 →
        /// VBO/IBO 子分配(含 IBO 失败时回滚 VBO 的事务处理)→
        /// CPU/GPU record 构建 → 段表 → 槽位分配。两个入口只在两点上不同:
        /// 段表 index_count 是否延后(见参数),以及尾部要不要排暂存拷贝。
        ///
        /// 抽出来的理由是**回滚只能有一份**:那段事务此前抄了两遍,改一处漏一处
        /// 就是永久性的竞技场区间泄漏。
        ///
        /// @param defer_index_count true = 段表 index_count 写 0(数据尚未落 GPU,
        ///        让 GPU 跳过该网格,等 markReady() 补真值);false = 立即写真值。
        Expected<MeshAllocResult> prepareMesh(const MeshCreateInfo& ci, bool defer_index_count);
        void setCapacityShortfall(
            lux::deployment::CapacityDomainIdView domain,
            std::uint64_t requested,
            std::uint64_t effective,
            std::uint64_t bytes,
            std::uint64_t available_bytes,
            lux::deployment::ECapacityPlanReason reason) noexcept;
        bool destroyNow(MeshHandle h);

        /// 把 IBO 字节范围换算成索引下标(可用作 VkDrawIndexedIndirectCommand::firstIndex)。
        ///
        /// ⚠ 返回值是 **段内相对** 的,不是跨段全局的 —— 旧名 calcIndexStartGlobal 里的
        ///   "Global" 是误导。`ir.offset` 来自 ChainedArenaAllocator,那是"段内字节偏移"
        ///   (见 SegmentedAllocation::offset),每段各自一条 VkBuffer 从 0 起算。所以段 1
        ///   里偏移 0 的网格,这里同样算出 0。要定位到真实位置,必须同时带上
        ///   MeshGpuRecord::ibo_segment;单看这个数字并按段 0 解释,就会读到别的网格的
        ///   索引(或越界)。GPU 驱动路径整趟 pass 只绑段 0,故在 serverAddMeshInstance
        ///   处拒绝非段 0 的网格,把这条不变量守住。
        static uint32_t calcIndexStartInSegment(EIndexType it, const BufferRange& ir, uint32_t sub_first)
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
        std::uint64_t                        ibo_topology_serial_{0u};
        std::uint64_t                        geometry_capacity_bytes_{0u};
        std::optional<lux::deployment::CapacityShortfall>
                                                last_capacity_shortfall_;

        // Segment table SSBO (geometry segment info; material related fields filled by upper layer later or use default on GPU side)
        SlicedSSBO<MeshInfoGpu>              segments_ssbo_;

        static constexpr std::size_t kMeshRecordsPerPage = 4096u;
        StableRecordPages<MeshCpuRecord, kMeshRecordsPerPage>
                                                cpu_records_;
        StableRecordPages<MeshGpuRecord, kMeshRecordsPerPage>
                                                gpu_records_;
        std::vector<uint32_t>                gens_;
        std::vector<uint32_t>                free_;
        std::vector<uint32_t>                instance_refcounts_;
        std::vector<uint8_t>                 destroy_requested_;
        // (这里曾有一个 std::atomic<uint32_t> slot_count_,注释说"游戏线程 release
        //  写、渲染线程 acquire 读",用来避开 push_back 改 vector 大小与 .size()
        //  并发读的竞争。那个游戏线程不存在:这些记录只在 server/render 线程上被
        //  分配与读取,slot_count_ 恒等于 cpu_records_.size(),已直接用后者取代。)

        /// CapacityPlan admission ceiling. It is not an implementation ceiling:
        /// cpu_records_/gpu_records_ grow by stable 4096-record pages, so adding a
        /// page cannot invalidate pointers into any earlier page.
        uint32_t                             mesh_max_count_ = 0;

        /// A VBO/IBO arena range awaiting frames-in-flight retirement. destroy()
        /// must NOT return the range to the arena immediately: a destroy-then-
        /// create within FIF frames could re-allocate the identical bytes and the
        /// async transfer-queue write would corrupt vertices that in-flight
        /// graphics frames are still drawing. Freed in retireFrameStagingBuffers()
        /// once the slot's fence has been waited.
        struct RetiredArenaRange {
            bool                 is_vbo{true};
            uint16_t             segment{0};
            BufferRange          range{};
            VmaVirtualAllocation handle{nullptr};
        };
        std::vector<std::vector<RetiredArenaRange>>       retired_ranges_;

        /// 段表槽的延迟归还。理由与 RetiredArenaRange 完全相同:立刻还回去,
        /// destroy→create 会复用同一个槽并覆写它的 GPU 条目,而 N-1/N-2 帧
        /// 可能仍按旧 segment_slot 读段表。走同一个 FIF 环、同一处 drain。
        ///(此前这里根本没有归还 —— 段表槽只分配不回收,每轮 create/destroy
        /// 永久多占一个,是单向泄漏。)
        std::vector<std::vector<SlotHandle>>             retired_segment_slots_;

        /// Pending acquire barriers for QFOT (render-thread only, drained per frame).
        std::vector<VkBufferMemoryBarrier2>               pending_acquire_barriers_;

        /// Pending staging copies for StagingOnly fallback (render-thread only).
        std::vector<PendingStagingCopy>                   pending_staging_copies_;

        uint32_t current_fi_{0};
    };

    class RenderContext;

    /// 惰性 + 幂等地建好**全局**网格竞技场(顶点 64MB + 索引 32MB)并接好线
    /// (每帧维护钩子、延迟销毁队列)。已建则立即返回。
    ///
    /// 谁调用:装 StandardMeshStack 特性时(L4),以及网格上传的装配路径(L6)。
    /// **"装这个特性"就是对这块 96MB 的 opt-in** —— 从不装它、也从不传网格的
    /// 服务器(纯 2D / headless / 只算 compute)一个字节都不分配。
    ///
    /// 定义在 src/render/resources/mesh/MeshResources.cpp(L3)。它以前住在 L6 的
    /// assembly TU 里,由 L4 前向声明后调用 —— 那是一条 **L4→L6 向上两层的链接期
    /// 依赖**,而层规则只看 include,一条都看不见。函数体里没有一个协议词汇,
    /// 它做的全是 L3 的事,所以归位到这里。
    ///
    /// 失败时对象留在注册表里但**未初始化**(注册表无 erase,重 emplace 会漏槽;
    /// 全局竞技场分配失败等同致命,不重试)。所有消费者经 isInitialized() 守卫。
    [[nodiscard]] LUX_FUNCTION_PUBLIC Expected<void> ensureGlobalMeshResources(RenderContext& ctx);

} // namespace lux::render
