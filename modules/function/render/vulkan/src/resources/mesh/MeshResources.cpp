#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/gpu/lifecycle/VRAMBudgetGuard.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp> // ensureGlobalMeshResources
#include <lux/engine/math/MeshBVH.hpp>

#include <vk_mem_alloc.h>
#include <lux/engine/render/gpu/VulkanCheck.hpp>
#include <cstring>
#include <numeric>

namespace lux::render
{
    void MeshResources::setCapacityShortfall(
        lux::render::CapacityDomainIdView domain,
        std::uint64_t requested,
        std::uint64_t effective,
        std::uint64_t bytes,
        std::uint64_t available_bytes,
        lux::render::CapacityPlanReason reason
    ) noexcept
    {
        last_capacity_shortfall_ = lux::render::CapacityShortfall{
            lux::render::CapacityDomainId{domain.name()},
            lux::render::CapacityPlanError::BUDGET_LIMIT,
            reason,
            requested,
            effective,
            bytes,
            available_bytes};
    }

    bool MeshResources::init(const InitInfo& ci)
    {
        device_ctx_ = ci.device;

        // Set BEFORE anything is allocated, not after. ~MeshResources() and
        // shutdown() both early-out on this flag, so a failure partway through
        // (the second createArena, say) used to leave the FIRST 64 MB arena
        // unreclaimable by anyone — nothing could free it. Flag first, and every
        // `return false` below reclaims through shutdown().
        initialized_ = true;

        vbo_segment_cap_ = ci.vertex_arena_bytes;
        ibo_segment_cap_ = ci.index_arena_bytes;
        geometry_capacity_bytes_ = ci.geometry_capacity_bytes;
        if (geometry_capacity_bytes_ < ci.vertex_arena_bytes + ci.index_arena_bytes)
        {
            shutdown();
            return false;
        }

        // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT is required so the VBO can be
        // exposed via the VertexPoolRegistry bindless SSBO array (R1.4 of
        // render-refactor). Mesh shaders read vertices via vertex attributes
        // today AND via storage buffer indexing after R5; both are valid
        // simultaneously, the bit just opts in to the SSBO view.
        vbo_usage_flags_ = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           (ci.enable_device_address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        ibo_usage_flags_ = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           (ci.enable_device_address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);

        // VBO segment 0
        {
            VkBuffer buf;
            VmaAllocation alloc;
            if (!createArena(ci.vertex_arena_bytes, vbo_usage_flags_, buf, alloc))
            {
                shutdown();
                return false;
            }
            vbo_buffers_.push_back(buf);
            vbo_allocs_.push_back(alloc);
        }

        // IBO segment 0
        {
            VkBuffer buf;
            VmaAllocation alloc;
            if (!createArena(ci.index_arena_bytes, ibo_usage_flags_, buf, alloc))
            {
                shutdown();
                return false;
            }
            ibo_buffers_.push_back(buf);
            ibo_allocs_.push_back(alloc);
        }

        // DESIGN-04: Initialize chained free-list arenas with first segment
        vbo_arena_.addSegment(ci.vertex_arena_bytes);
        ibo_arena_.addSegment(ci.index_arena_bytes);

        // PERF-01: Create reusable transfer command pool + fence
        // NOTE: removed – all Vulkan queue ops now happen on the render thread
        //       (see retireFrameStagingBuffers).

        // Reserve the per-frame-in-flight retirement rings
        retired_ranges_.resize(ci.frames_in_flight);
        retired_segment_slots_.resize(ci.frames_in_flight);

        // Reserve the page-pointer table and scalar metadata. Record storage
        // itself is allocated in stable 4096-record pages.
        cpu_records_.reserve(ci.mesh_max_count);
        gpu_records_.reserve(ci.mesh_max_count);
        gens_.reserve(ci.mesh_max_count);
        instance_refcounts_.reserve(ci.mesh_max_count);
        destroy_requested_.reserve(ci.mesh_max_count);
        handle_assets_.reserve(ci.mesh_max_count);
        mesh_max_count_ = ci.mesh_max_count; // enforced in create()/allocateOnly() (C-4)

        // Segment table (read-only, MeshResources handles the geometry part)
        SSBOInitConfig seg_cfg = ci.segments_ssbo_cfg;
        if (!seg_cfg.device_context)
            seg_cfg.device_context = device_ctx_;
        if (seg_cfg.initial_dense_capacity == 0)
            seg_cfg.initial_dense_capacity = 16384;
        if (seg_cfg.slices == 0)
            seg_cfg.slices = 1;
        seg_cfg.allow_shader_write = false;
        segments_ssbo_.init(seg_cfg);

        return true; // initialized_ was set at the top — see the note there
    }

    MeshResources::~MeshResources()
    {
        if (initialized_)
            shutdown();
    }

    void MeshResources::shutdown()
    {
        if (!initialized_)
            return;
        initialized_ = false;

        auto vma = device_ctx_->vmaAllocator();
        cpu_records_.clear();
        gpu_records_.clear();
        gens_.clear();
        free_.clear();
        instance_refcounts_.clear();
        destroy_requested_.clear();
        asset_handles_.clear();
        handle_assets_.clear();
        segments_ssbo_.destroy();

        // Destroy all VBO/IBO segments
        for (size_t i = 0; i < vbo_buffers_.size(); ++i)
            vmaDestroyBuffer(vma, vbo_buffers_[i], vbo_allocs_[i]);
        vbo_buffers_.clear();
        vbo_allocs_.clear();

        for (size_t i = 0; i < ibo_buffers_.size(); ++i)
            vmaDestroyBuffer(vma, ibo_buffers_[i], ibo_allocs_[i]);
        ibo_buffers_.clear();
        ibo_allocs_.clear();

        // Arena ranges awaiting FIF retirement: the arenas are torn down below,
        // so just drop the bookkeeping (GPU is idle at shutdown).
        retired_ranges_.clear();

        vbo_arena_.clear();
        ibo_arena_.clear();
        vbo_segment_cap_ = ibo_segment_cap_ = 0;
        vbo_usage_flags_ = ibo_usage_flags_ = 0;
        ibo_topology_serial_ = 0u;
        geometry_capacity_bytes_ = 0u;
        last_capacity_shortfall_.reset();
    }

    // ---------- create ----------
    // Fill the per-LOD index sub-ranges on a freshly-built GPU record from the
    // create-info's LOD counts (LOD0 first). Shared by create() + allocateOnly().
    static void fillLodRanges(MeshGpuRecord& gpu, const MeshCreateInfo& ci, uint32_t index_size)
    {
        // 段内相对起点(不是跨段全局下标):index_buffer_range.offset 是 ibo_segment
        // 那一段内的字节偏移。见 MeshResources::calcIndexStartInSegment 的说明。
        const uint32_t seg_first = static_cast<uint32_t>(gpu.index_buffer_range.offset / index_size);
        if (ci.lod_index_counts.empty())
        {
            gpu.lod_count = 1;
            gpu.lod_index_first[0] = seg_first;
            gpu.lod_index_count[0] = gpu.index_count;
            return;
        }
        uint32_t n = static_cast<uint32_t>(ci.lod_index_counts.size());
        if (n > kMaxMeshLod)
            n = kMaxMeshLod;
        uint32_t cum = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            gpu.lod_index_first[i] = seg_first + cum;
            gpu.lod_index_count[i] = ci.lod_index_counts[i];
            cum += ci.lod_index_counts[i];
        }
        gpu.lod_count = static_cast<uint8_t>(n);
    }

    // ---------- prepareMesh (create / allocateOnly 的公共前段) ----------
    //
    // create() 与 allocateOnly() 曾把下面整段逐字抄了两份,真实差异只有两点:
    // 段表里的 index_count 写实值还是 0,以及尾部是"排一次暂存拷贝"还是"把
    // 分配结果交给异步 worker"。抄两份的危险不在行数,而在**回滚**:IBO 子分配
    // 失败时必须回滚已成功的 VBO 区间,这段事务逻辑此前有两份,
    // 改一处漏一处就是永久性的竞技场区间泄漏。
    //
    // @param defer_index_count true = 段表 index_count 写 0(GPU 因此跳过尚未
    //        就绪的网格,等 markReady() 补真值);false = 立即写真值。
    Expected<MeshAllocResult> MeshResources::prepareMesh(const MeshCreateInfo& ci, bool defer_index_count)
    {
        last_capacity_shortfall_.reset();
        if (ci.layout_id == invalid_layout_id || ci.vertex_stride == 0)
            return renderFailure<err::internal::InvalidArgument>();

        // CapacityPlan admission ceiling. CPU/GPU records themselves live in
        // stable 4096-record pages, so this is no longer a pointer-stability cap.
        if (free_.empty() && cpu_records_.size() >= mesh_max_count_)
        {
            const auto record_bytes =
                sizeof(MeshCpuRecord) + sizeof(MeshGpuRecord) + sizeof(std::uint32_t) * 3u + sizeof(std::uint8_t);
            setCapacityShortfall(
                lux::render::kClassicMeshRecordsCapacity,
                static_cast<std::uint64_t>(cpu_records_.size()) + 1u,
                mesh_max_count_,
                record_bytes,
                0u,
                lux::render::CapacityPlanReason::BUDGET_REJECT
            );
            return renderFailure<err::memory::OutOfMemory>();
        }

        // 1) Infer index_type
        EIndexType it = ci.index_type;
        if (it == EIndexType::None)
        {
            if (!ci.index_buffer.empty())
            {
                const auto bytes = ci.index_buffer.size_bytes();
                if (bytes % 4 == 0)
                    it = EIndexType::UInt32;
                else if (bytes % 2 == 0)
                    it = EIndexType::UInt16;
                else
                    return renderFailure<err::internal::InvalidArgument>();
            }
            else
            {
                it = EIndexType::UInt32;
            }
        }

        // 2) VBO / IBO sub-allocation (DESIGN-04: ChainedArenaAllocator).
        // VBO alignment must be a multiple of vertex_stride so that
        // (offset / stride) yields an exact integer for base_vertex.
        const uint64_t vbo_align = std::lcm(uint64_t(256), uint64_t(ci.vertex_stride));
        const auto vbo_growth =
            !ci.vertex_buffer.empty() && !vbo_arena_.canAllocate(ci.vertex_buffer.size_bytes(), vbo_align)
                ? std::max(vbo_segment_cap_, static_cast<VkDeviceSize>(ci.vertex_buffer.size_bytes()))
                : 0u;
        const auto ibo_growth =
            !ci.index_buffer.empty() && !ibo_arena_.canAllocate(ci.index_buffer.size_bytes(), 256u)
                ? std::max(ibo_segment_cap_, static_cast<VkDeviceSize>(ci.index_buffer.size_bytes()))
                : 0u;
        const auto growth_bytes = vbo_growth + ibo_growth;
        const auto committed_geometry = vbo_arena_.totalCapacity() + ibo_arena_.totalCapacity();
        if (growth_bytes > geometry_capacity_bytes_ || committed_geometry > geometry_capacity_bytes_ - growth_bytes)
        {
            setCapacityShortfall(
                lux::render::kClassicMeshGeometryBytesCapacity,
                committed_geometry + growth_bytes,
                geometry_capacity_bytes_,
                growth_bytes,
                geometry_capacity_bytes_ > committed_geometry ? geometry_capacity_bytes_ - committed_geometry : 0u,
                lux::render::CapacityPlanReason::BUDGET_REJECT
            );
            return renderFailure<err::memory::OutOfMemory>();
        }
        if (growth_bytes != 0u)
        {
            VRAMBudgetGuard budget(device_ctx_->vmaAllocator());
            if (!budget.canAllocate(growth_bytes))
            {
                const auto snapshot = budget.snapshot();
                setCapacityShortfall(
                    lux::render::kClassicMeshGeometryBytesCapacity,
                    committed_geometry + growth_bytes,
                    geometry_capacity_bytes_,
                    growth_bytes,
                    snapshot.total_budget > snapshot.total_usage ? snapshot.total_budget - snapshot.total_usage : 0u,
                    lux::render::CapacityPlanReason::BUDGET_REJECT
                );
                return renderFailure<err::memory::OutOfMemory>();
            }
        }
        const auto old_vbo_segments = vbo_arena_.segmentCount();
        const auto old_ibo_segments = ibo_arena_.segmentCount();
        auto vrExp = suballoc(vbo_arena_, ci.vertex_buffer, vbo_align);
        if (!vrExp)
        {
            rollbackUnpublishedSegments(old_vbo_segments, old_ibo_segments);
            return lux::cxx::unexpected(vrExp.error());
        }
        auto irExp = suballoc(ibo_arena_, ci.index_buffer);
        if (!irExp)
        {
            // Roll back the VBO suballocation that already succeeded — otherwise a failed
            // IBO suballoc permanently leaks the VBO arena range. The completion-level
            // rollback (reclaimReservedSlot) cannot cover this: no task or completion
            // exists yet, so this must be transactional here.
            vbo_arena_.free({vrExp->segment, vrExp->range.offset, vrExp->range.size, vrExp->alloc_handle});
            rollbackUnpublishedSegments(old_vbo_segments, old_ibo_segments);
            return lux::cxx::unexpected(irExp.error());
        }

        const uint32_t index_size = (it == EIndexType::UInt16 ? 2u : 4u);

        // 3) Build CPU and GPU records
        MeshCpuRecord cpu{};
        cpu.local_bounds = ci.bounds.value_or(math::AABB{});
        cpu.valid = true;

        MeshGpuRecord gpu{};
        gpu.layout_id = ci.layout_id;
        gpu.vertex_stride = ci.vertex_stride;
        gpu.vertex_buffer_range = vrExp->range;
        gpu.index_buffer_range = irExp->range;
        gpu.vbo_segment = vrExp->segment;
        gpu.ibo_segment = irExp->segment;
        gpu.vbo_alloc_handle = vrExp->alloc_handle;
        gpu.ibo_alloc_handle = irExp->alloc_handle;
        gpu.index_type = it;
        gpu.index_count = static_cast<uint32_t>(ci.index_buffer.size_bytes() / index_size);
        gpu.ready = false;
        fillLodRanges(gpu, ci, index_size);

        // 4) Segment table: treat the entire index_buffer as one segment.
        MeshInfoGpu seg{};
        seg.index_first = calcIndexStartInSegment(it, gpu.index_buffer_range, /*sub_first=*/0);
        seg.index_count =
            defer_index_count
                ? 0u // GPU skips non-ready meshes until markReady() writes the real count
                : (ci.index_buffer.empty() ? 0u : static_cast<uint32_t>(ci.index_buffer.size_bytes() / index_size));
        seg.base_vertex = static_cast<int32_t>(gpu.vertex_buffer_range.offset / gpu.vertex_stride);

        if (ci.bounds.has_value())
        {
            std::memcpy(seg.bounds_min, ci.bounds->min.data(), sizeof(float) * 3);
            std::memcpy(seg.bounds_max, ci.bounds->max.data(), sizeof(float) * 3);
        }
        else
        {
            seg.bounds_min[0] = seg.bounds_min[1] = seg.bounds_min[2] = +FLT_MAX;
            seg.bounds_max[0] = seg.bounds_max[1] = seg.bounds_max[2] = -FLT_MAX;
        }

        gpu.segment_slot = segments_ssbo_.add(seg);
        if (!gpu.segment_slot.isValid())
        {
            vbo_arena_.free({vrExp->segment, vrExp->range.offset, vrExp->range.size, vrExp->alloc_handle});
            ibo_arena_.free({irExp->segment, irExp->range.offset, irExp->range.size, irExp->alloc_handle});
            rollbackUnpublishedSegments(old_vbo_segments, old_ibo_segments);
            setCapacityShortfall(
                lux::render::kClassicMeshRecordsCapacity,
                static_cast<std::uint64_t>(cpu_records_.size()) + 1u,
                mesh_max_count_,
                sizeof(MeshInfoGpu) * segments_ssbo_.slices(),
                mesh_max_count_ > cpu_records_.size()
                    ? (mesh_max_count_ - cpu_records_.size()) * sizeof(MeshInfoGpu) * segments_ssbo_.slices()
                    : 0u,
                lux::render::CapacityPlanReason::BUDGET_REJECT
            );
            return renderFailure<err::memory::OutOfMemory>();
        }

        // 5) Handle allocation — slot reuse from free list or new push_back
        uint32_t id;
        if (!free_.empty())
        {
            id = free_.back();
            free_.pop_back();
            cpu_records_[id] = std::move(cpu);
            gpu_records_[id] = std::move(gpu);
            instance_refcounts_[id] = 0u;
            destroy_requested_[id] = 0u;
        }
        else
        {
            id = static_cast<uint32_t>(cpu_records_.size());
            cpu_records_.push_back(std::move(cpu));
            gpu_records_.push_back(std::move(gpu));
            gens_.push_back(1);
            instance_refcounts_.push_back(0u);
            destroy_requested_.push_back(0u);
        }

        return MeshAllocResult{MeshHandle{id, gens_[id]}, vrExp->range, irExp->range, vrExp->segment, irExp->segment};
    }

    // ---------- allocateOnly (异步路径 — 不排暂存拷贝,worker 直接写) ----------
    Expected<MeshAllocResult> MeshResources::allocateOnly(const MeshCreateInfo& ci)
    {
        // index_count 延后到 markReady():数据还没落 GPU,段表先写 0 让 GPU 跳过。
        return prepareMesh(ci, /*defer_index_count=*/true);
    }

    // ---------- markReady ----------
    void MeshResources::markReady(uint32_t mesh_index)
    {
        auto& gpu = gpu_records_[mesh_index];

        // Restore the real index_count in the segment table so the GPU cull
        // shader starts generating draw commands for this mesh.
        if (auto* seg = segments_ssbo_.get(gpu.segment_slot))
        {
            seg->index_count = gpu.index_count;
            segments_ssbo_.touch(gpu.segment_slot);
        }

        gpu.ready = true;
    }

    bool MeshResources::retainForInstance(MeshHandle h) noexcept
    {
        if (!alive(h) || destroy_requested_[h.index] != 0u)
            return false;
        ++instance_refcounts_[h.index];
        return true;
    }

    void MeshResources::releaseFromInstance(MeshHandle h) noexcept
    {
        if (!alive(h) || instance_refcounts_[h.index] == 0u)
            return;
        --instance_refcounts_[h.index];
        if (instance_refcounts_[h.index] == 0u && destroy_requested_[h.index] != 0u)
        {
            (void)destroyNow(h);
        }
    }

    bool MeshResources::destroy(MeshHandle h)
    {
        if (!alive(h))
            return false;
        if (instance_refcounts_[h.index] != 0u)
        {
            destroy_requested_[h.index] = 1u;
            return true;
        }
        return destroyNow(h);
    }

    bool MeshResources::destroyNow(MeshHandle h)
    {
        if (!alive(h))
            return false;
        auto& gpu = gpu_records_[h.index];
        // DESIGN-04: Free arena space (VBO/IBO) so it can be reused — but DEFER it
        // by frames-in-flight. Returning the range immediately lets a destroy-then-
        // create within ~2 frames re-allocate the identical bytes, and the async
        // transfer-queue write into them races the still-executing graphics frames
        // (N-1/N-2) that draw the destroyed mesh's old vertices. Retire into the
        // per-FIF ring; retireFrameStagingBuffers() returns them to the arena after
        // the slot fence is waited.
        if (gpu.vertex_buffer_range.isValid())
            retired_ranges_[current_fi_].push_back(
                RetiredArenaRange{true, gpu.vbo_segment, gpu.vertex_buffer_range, gpu.vbo_alloc_handle}
            );
        if (gpu.index_buffer_range.isValid())
            retired_ranges_[current_fi_].push_back(
                RetiredArenaRange{false, gpu.ibo_segment, gpu.index_buffer_range, gpu.ibo_alloc_handle}
            );
        // 段表槽同样延迟归还(见 retired_segment_slots_ 的说明)——
        // 此前这里完全没有归还,是单向泄漏。
        retired_segment_slots_[current_fi_].push_back(gpu.segment_slot);
        cpu_records_[h.index].valid = false;
        if (h.index < handle_assets_.size() && !handle_assets_[h.index].isNull())
        {
            asset_handles_.erase(handle_assets_[h.index]);
            handle_assets_[h.index] = asset::NullAssetId;
        }
        instance_refcounts_[h.index] = 0u;
        destroy_requested_[h.index] = 0u;
        gens_[h.index]++;
        free_.push_back(h.index);
        return true;
    }

    bool MeshResources::alive(MeshHandle h) const
    {
        return h.isValid() && h.index < cpu_records_.size() && cpu_records_[h.index].valid && gens_[h.index] == h.gen;
    }

    std::optional<MeshHandle> MeshResources::findAsset(asset::AssetId id) const noexcept
    {
        if (id.isNull())
        {
            return std::nullopt;
        }
        const auto found = asset_handles_.find(id);
        if (found == asset_handles_.end() || !alive(found->second))
        {
            return std::nullopt;
        }
        return found->second;
    }

    bool MeshResources::bindAsset(asset::AssetId id, MeshHandle handle)
    {
        if (id.isNull())
        {
            return true;
        }
        if (!alive(handle) || asset_handles_.contains(id))
        {
            return false;
        }
        if (handle_assets_.size() <= handle.index)
        {
            handle_assets_.resize(static_cast<std::size_t>(handle.index) + 1U);
        }
        asset_handles_.emplace(id, handle);
        handle_assets_[handle.index] = id;
        return true;
    }

    Expected<VertexLayoutId> MeshResources::layoutId(MeshHandle h) const
    {
        if (!alive(h))
            return renderFailure<err::resource::NotFound>();
        return gpu_records_[h.index].layout_id;
    }

    // DESIGN-04: Free-list based sub-allocation (replaces bump cursor)
    Expected<MeshResources::SegmentedRange>
    MeshResources::suballoc(ChainedArenaAllocator& arena, std::span<const std::byte> data, uint64_t alignment)
    {
        if (data.empty())
            return SegmentedRange{};
        const uint64_t need = data.size_bytes();
        auto alloc = arena.allocate(need, alignment);
        if (!alloc.valid())
        {
            // Auto-grow: add a new buffer segment and retry.
            const bool is_vbo = (&arena == &vbo_arena_);
            const VkDeviceSize seg_cap = is_vbo ? vbo_segment_cap_ : ibo_segment_cap_;
            const VkDeviceSize grow_bytes = std::max(seg_cap, static_cast<VkDeviceSize>(need));

            const auto committed_geometry = vbo_arena_.totalCapacity() + ibo_arena_.totalCapacity();
            if (grow_bytes > geometry_capacity_bytes_ || committed_geometry > geometry_capacity_bytes_ - grow_bytes)
            {
                setCapacityShortfall(
                    lux::render::kClassicMeshGeometryBytesCapacity,
                    committed_geometry + grow_bytes,
                    geometry_capacity_bytes_,
                    grow_bytes,
                    geometry_capacity_bytes_ > committed_geometry ? geometry_capacity_bytes_ - committed_geometry : 0u,
                    lux::render::CapacityPlanReason::BUDGET_REJECT
                );
                return renderFailure<err::memory::OutOfMemory>();
            }

            VRAMBudgetGuard budget(device_ctx_->vmaAllocator());
            if (!budget.canAllocate(grow_bytes))
            {
                const auto snapshot = budget.snapshot();
                setCapacityShortfall(
                    lux::render::kClassicMeshGeometryBytesCapacity,
                    committed_geometry + grow_bytes,
                    geometry_capacity_bytes_,
                    grow_bytes,
                    snapshot.total_budget > snapshot.total_usage ? snapshot.total_budget - snapshot.total_usage : 0u,
                    lux::render::CapacityPlanReason::BUDGET_REJECT
                );
                return renderFailure<err::memory::OutOfMemory>();
            }

            auto& bufs = is_vbo ? vbo_buffers_ : ibo_buffers_;
            auto& allocs_vec = is_vbo ? vbo_allocs_ : ibo_allocs_;
            const VkBufferUsageFlags usage = is_vbo ? vbo_usage_flags_ : ibo_usage_flags_;

            if (!addBufferSegment(grow_bytes, usage, bufs, allocs_vec, arena))
            {
                const auto snapshot = budget.snapshot();
                setCapacityShortfall(
                    lux::render::kClassicMeshGeometryBytesCapacity,
                    committed_geometry + grow_bytes,
                    geometry_capacity_bytes_,
                    grow_bytes,
                    snapshot.total_budget > snapshot.total_usage ? snapshot.total_budget - snapshot.total_usage : 0u,
                    lux::render::CapacityPlanReason::BUDGET_REJECT
                );
                return renderFailure<err::memory::OutOfMemory>();
            }

            alloc = arena.allocate(need, alignment);
            if (!alloc.valid())
                return renderFailure<err::memory::OutOfMemory>();
        }
        return SegmentedRange{BufferRange{alloc.offset, alloc.size}, alloc.segment_index, alloc.handle};
    }

    bool
    MeshResources::createArena(VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer& out, VmaAllocation& out_alloc)
    {
        auto vma = device_ctx_->vmaAllocator();

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = usage;
        const std::array queue_families{
            device_ctx_->graphicsQueueFamilyIndex(),
            device_ctx_->transferQueueFamilyIndex()};
        if (queue_families[0] != queue_families[1])
        {
            // Mesh arenas are append-heavy, long-lived buffers consumed by the
            // graphics queue while independent ranges continue uploading on the
            // transfer queue. Concurrent sharing is the honest ownership model;
            // per-range EXCLUSIVE QFOTs cannot transfer the same arena between
            // both families without serializing all resident mesh users.
            bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
            bi.queueFamilyIndexCount = static_cast<std::uint32_t>(queue_families.size());
            bi.pQueueFamilyIndices = queue_families.data();
        }
        else
        {
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            bi.queueFamilyIndexCount = 0;
            bi.pQueueFamilyIndices = nullptr;
        }

        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.flags = 0;
        ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        VkResult result = vmaCreateBuffer(vma, &bi, &ai, &out, &out_alloc, nullptr);
        return result == VK_SUCCESS;
    }

    bool MeshResources::addBufferSegment(
        VkDeviceSize bytes,
        VkBufferUsageFlags usage,
        std::vector<VkBuffer>& bufs,
        std::vector<VmaAllocation>& allocs,
        ChainedArenaAllocator& arena
    )
    {
        VkBuffer buf;
        VmaAllocation alloc;
        if (!createArena(bytes, usage, buf, alloc))
            return false;
        bufs.push_back(buf);
        allocs.push_back(alloc);
        auto idx = arena.addSegment(bytes);
        if (idx == ChainedArenaAllocator::kInvalidSegment)
        {
            bufs.pop_back();
            allocs.pop_back();
            vmaDestroyBuffer(device_ctx_->vmaAllocator(), buf, alloc);
            return false;
        }
        if (&arena == &ibo_arena_)
            ++ibo_topology_serial_;
        return true;
    }

    void MeshResources::rollbackUnpublishedSegments(
        std::uint16_t vbo_segment_count,
        std::uint16_t ibo_segment_count
    ) noexcept
    {
        auto rollback = [this](auto& arena, auto& buffers, auto& allocations, std::uint16_t count) {
            while (arena.segmentCount() > count)
            {
                if (!arena.removeLastEmptySegment())
                    break;
                const auto buffer = buffers.back();
                const auto allocation = allocations.back();
                buffers.pop_back();
                allocations.pop_back();
                vmaDestroyBuffer(device_ctx_->vmaAllocator(), buffer, allocation);
            }
        };
        rollback(vbo_arena_, vbo_buffers_, vbo_allocs_, vbo_segment_count);
        const auto before = ibo_arena_.segmentCount();
        rollback(ibo_arena_, ibo_buffers_, ibo_allocs_, ibo_segment_count);
        ibo_topology_serial_ -= before - ibo_arena_.segmentCount();
    }

    // Render-thread: called in beginFrame() after waiting on the frame fence.
    void MeshResources::retireFrameStagingBuffers(uint32_t fi)
    {
        // The slot's fence has been waited (called from onBeginFrame), so any
        // frame that referenced these arena ranges is complete — return them to
        // the arena now, safe to re-allocate. (#23)
        auto& ranges = retired_ranges_[fi];
        for (const auto& r : ranges)
        {
            if (!r.range.isValid())
                continue;
            if (r.is_vbo)
                vbo_arena_.free({r.segment, r.range.offset, r.range.size, r.handle});
            else
                ibo_arena_.free({r.segment, r.range.offset, r.range.size, r.handle});
        }
        ranges.clear();

        // 段表槽:fence 已等过,复用它不再会撞上在途帧读旧条目。
        auto& seg_slots = retired_segment_slots_[fi];
        for (const auto& sh : seg_slots)
            segments_ssbo_.remove(sh); // 无效句柄由 remove 自身的 isAlive 守卫过滤
        seg_slots.clear();
    }

    // ---------- Async acquire barriers ----------

    void MeshResources::pushAcquireBarrier(
        VkBuffer buf,
        VkDeviceSize offset,
        VkDeviceSize size,
        uint32_t src_queue_family,
        uint32_t dst_queue_family
    )
    {
        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.srcAccessMask = VK_ACCESS_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
        b.srcQueueFamilyIndex = src_queue_family;
        b.dstQueueFamilyIndex = dst_queue_family;
        b.buffer = buf;
        b.offset = offset;
        b.size = size;
        pending_acquire_barriers_.push_back(b);
    }

    void MeshResources::recordAcquireBarriers(VkCommandBuffer cmd)
    {
        if (pending_acquire_barriers_.empty())
            return;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount = static_cast<uint32_t>(pending_acquire_barriers_.size());
        dep.pBufferMemoryBarriers = pending_acquire_barriers_.data();
        vkCmdPipelineBarrier2(cmd, &dep);
        pending_acquire_barriers_.clear();
    }

    // ---------- StagingOnly fallback ----------

    void MeshResources::pushStagingCopy(const PendingStagingCopy& copy)
    {
        pending_staging_copies_.push_back(copy);
    }

    void MeshResources::recordStagingCopies(VkCommandBuffer cmd)
    {
        if (pending_staging_copies_.empty())
            return;

        for (auto& c : pending_staging_copies_)
        {
            if (c.vbo_size > 0)
            {
                VkBufferCopy region{c.vbo_stg_offset, c.vbo_dst_offset, c.vbo_size};
                vkCmdCopyBuffer(cmd, c.stg_buf, c.vbo_dst, 1, &region);
            }
            if (c.ibo_size > 0)
            {
                VkBufferCopy region{c.ibo_stg_offset, c.ibo_dst_offset, c.ibo_size};
                vkCmdCopyBuffer(cmd, c.stg_buf, c.ibo_dst, 1, &region);
            }
        }

        // Execution barrier: copy writes → vertex/index reads
        VkMemoryBarrier2 bar{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        bar.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        bar.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        bar.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &bar;
        vkCmdPipelineBarrier2(cmd, &dep);

        for (auto& c : pending_staging_copies_)
            markReady(c.mesh_index);
        pending_staging_copies_.clear();
    }

    // ── TransferScheduler integration ──

    void MeshResources::submitTransfers(TransferScheduler& scheduler)
    {
        // StagingOnly fallback copies.
        for (auto& c : pending_staging_copies_)
        {
            if (c.vbo_size > 0)
            {
                scheduler.submitBufferCopy({
                    .src = c.stg_buf,
                    .src_offset = c.vbo_stg_offset,
                    .dst = c.vbo_dst,
                    .dst_offset = c.vbo_dst_offset,
                    .size = c.vbo_size,
                    .domain = EBufferDomain::VertexInput,
                }
                );
            }
            if (c.ibo_size > 0)
            {
                scheduler.submitBufferCopy({
                    .src = c.stg_buf,
                    .src_offset = c.ibo_stg_offset,
                    .dst = c.ibo_dst,
                    .dst_offset = c.ibo_dst_offset,
                    .size = c.ibo_size,
                    .domain = EBufferDomain::VertexInput,
                }
                );
            }
            markReady(c.mesh_index);
        }
        pending_staging_copies_.clear();

        // 3. QFOT acquire barriers.
        for (auto& b : pending_acquire_barriers_)
        {
            scheduler.submitQFOTAcquire({
                .kind = QFOTAcquireRequest::Kind::Buffer,
                .buffer = b.buffer,
                .buf_offset = b.offset,
                .buf_size = b.size,
                .src_family = b.srcQueueFamilyIndex,
                .dst_family = b.dstQueueFamilyIndex,
                .domain = EBufferDomain::VertexInput,
            }
            );
        }
        pending_acquire_barriers_.clear();
    }

    // ── 全局网格竞技场的惰性构造器 ──────────────────────────────────────
    //
    // 从 L6 的 assembly/meshstack/MeshStackOperationHandlers.cpp 搬来。它长在那里
    // 的唯一原因是历史:当初从 RenderServer.cpp 搬出去时跟着 handler 一起走了。
    // 但它的函数体里没有一个协议词汇 —— 建 InitInfo、emplace、挂每帧维护钩子、
    // init、接延迟销毁队列,全是 L3 的事。留在 L6 的代价是 L4 的
    // StandardMeshStackFeature 必须前向声明它,形成一条 **L4→L6 向上两层的
    // 链接期依赖**,而层规则只看 include,一条都看不见。
    Expected<void> ensureGlobalMeshResources(RenderContext& ctx)
    {
        auto& greg = ctx.globalRegistry();
        if (greg.find<MeshResources>())
            return {};

        MeshResources::InitInfo info{
            .device = &ctx.deviceContext(),
            .vertex_arena_bytes = 64ull * 1024 * 1024,
            .index_arena_bytes = 32ull * 1024 * 1024,
            .enable_device_address =
                ctx.capacityPlan().device.buffer_device_address && ctx.capacityPlan().device.shader_int64,
            .frames_in_flight = ctx.framesInFlight(),
            .mesh_max_count =
                static_cast<std::uint32_t>(ctx.capacityPlan().effective(lux::render::kClassicMeshRecordsCapacity)),
            .geometry_capacity_bytes = ctx.capacityPlan().effective(lux::render::kClassicMeshGeometryBytesCapacity),
            .segments_ssbo_cfg =
                SSBOInitConfig{
                    .device_context = &ctx.deviceContext(),
                    .initial_dense_capacity = 16384,
                    .slices = ctx.framesInFlight(),
                    .allow_shader_write = false,
                    .clear_on_remove = false,
                },
        };
        // ensure<T>(init_args):构造 + init + **只在成功时发布**。此前是 emplace →
        // find → init,失败时那个未初始化的对象留在注册表里(无 erase API,重新
        // emplace 会漏一个槽),于是「不重试」成了唯一选择,而每个消费者都得自己
        // 守 isInitialized()。现在失败什么都不留,下一次调用自然重试。
        auto mr_r = greg.ensure<MeshResources>(info);
        if (!mr_r)
            return lux::cxx::unexpected<RenderError>(mr_r.error());
        auto* mr = *mr_r;

        // 每帧维护由**安装点**登记 —— 资源自己不再继承帧接口。登记必须在 ensure
        // 成功**之后**:失败即不发布意味着失败对象随即销毁,而注册表没有
        // removeBeginFrameHook —— 早登记的钩子捕获的裸指针会成为每帧一次的
        // use-after-free。
        greg.addBeginFrameHook(EUploadPhase::Upload, [mr](const FrameStamp& s) { mr->onFrameBeginMaintenance(s); });
        ctx.globalTransferScheduler().contributors().add(makeTransferContributor(mr, /*priority=*/0));
        mr->setDeferredQueue(&ctx.deferredDestroyQueue());
        return {};
    }

} // namespace lux::render
