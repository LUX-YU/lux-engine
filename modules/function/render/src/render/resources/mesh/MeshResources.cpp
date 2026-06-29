#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/lifecycle/VRAMBudgetGuard.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>
#include <lux/engine/math/MeshBVH.hpp>

#include <vk_mem_alloc.h>
#include <lux/engine/render/core/VulkanCheck.hpp>
#include <cstring>
#include <numeric>

namespace lux::render
{
    bool MeshResources::init(const InitInfo& ci)
    {
        device_ctx_ = ci.device;

        vbo_segment_cap_ = ci.vertex_arena_bytes;
        ibo_segment_cap_ = ci.index_arena_bytes;

        // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT is required so the VBO can be
        // exposed via the VertexPoolRegistry bindless SSBO array (R1.4 of
        // render-refactor). Mesh shaders read vertices via vertex attributes
        // today AND via storage buffer indexing after R5; both are valid
        // simultaneously, the bit just opts in to the SSBO view.
        vbo_usage_flags_ = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           (ci.enable_device_address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        ibo_usage_flags_ = VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           (ci.enable_device_address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);

        // VBO segment 0
        {
            VkBuffer buf; VmaAllocation alloc;
            if (!createArena(ci.vertex_arena_bytes, vbo_usage_flags_, buf, alloc))
                return false;
            vbo_buffers_.push_back(buf);
            vbo_allocs_.push_back(alloc);
        }

        // IBO segment 0
        {
            VkBuffer buf; VmaAllocation alloc;
            if (!createArena(ci.index_arena_bytes, ibo_usage_flags_, buf, alloc))
                return false;
            ibo_buffers_.push_back(buf);
            ibo_allocs_.push_back(alloc);
        }

        // DESIGN-04: Initialize chained free-list arenas with first segment
        vbo_arena_.addSegment(ci.vertex_arena_bytes);
        ibo_arena_.addSegment(ci.index_arena_bytes);

        // PERF-01: Create reusable transfer command pool + fence
        // NOTE: removed – all Vulkan queue ops now happen on the render thread
        //       (see flushPendingTransfers / retireFrameStagingBuffers).

        // Reserve deferred-deletion ring (one slot per frame-in-flight)
        deferred_deletions_.resize(ci.frames_in_flight);
        retired_ranges_.resize(ci.frames_in_flight);

        // Pre-reserve cpu_records_, gpu_records_, and gens_ to ci.mesh_max_count so
        // that subsequent push_back() calls on the game thread NEVER trigger a
        // reallocation.  Reallocation while the render thread holds indices
        // into either vector would cause a use-after-free data race.
        cpu_records_.reserve(ci.mesh_max_count);
        gpu_records_.reserve(ci.mesh_max_count);
        gens_.reserve(ci.mesh_max_count);
        mesh_max_count_ = ci.mesh_max_count;   // enforced in create()/allocateOnly() (C-4)

        // Segment table (read-only, MeshResources handles the geometry part)
        SSBOInitConfig seg_cfg = ci.segments_ssbo_cfg;
        if (!seg_cfg.device_context) seg_cfg.device_context = device_ctx_;
        if (seg_cfg.initial_dense_capacity == 0) seg_cfg.initial_dense_capacity = 16384;
        if (seg_cfg.slices == 0) seg_cfg.slices = 1;
        seg_cfg.allow_shader_write = false;
        segments_ssbo_.init(seg_cfg);

        initialized_ = true;
        return true;
    }

    MeshResources::~MeshResources()
    {
        if (initialized_)
            shutdown();
    }

    void MeshResources::shutdown()
    {
        if (!initialized_) return;
        initialized_ = false;

        auto vma = device_ctx_->vmaAllocator();
        cpu_records_.clear();
        gpu_records_.clear();
        gens_.clear();
        free_.clear();
        segments_ssbo_.reset();

        // Destroy all VBO/IBO segments
        for (size_t i = 0; i < vbo_buffers_.size(); ++i)
            vmaDestroyBuffer(vma, vbo_buffers_[i], vbo_allocs_[i]);
        vbo_buffers_.clear();
        vbo_allocs_.clear();

        for (size_t i = 0; i < ibo_buffers_.size(); ++i)
            vmaDestroyBuffer(vma, ibo_buffers_[i], ibo_allocs_[i]);
        ibo_buffers_.clear();
        ibo_allocs_.clear();

        // Drain pending staging transfers that were never flushed
        {
            PendingTransfer t;
            while (pending_queue_.pop(t)) {
                if (t.stg_alloc)
                    vmaDestroyBuffer(vma, t.stg_buf, t.stg_alloc);
            }
        }

        // Drain any remaining deferred staging buffers (GPU is idle at shutdown)
        for (auto& slot : deferred_deletions_)
            slot.clear();
        deferred_deletions_.clear();
        // Arena ranges awaiting FIF retirement: the arenas are torn down below,
        // so just drop the bookkeeping (GPU is idle at shutdown).
        retired_ranges_.clear();

        vbo_arena_.clear();
        ibo_arena_.clear();
        vbo_segment_cap_ = ibo_segment_cap_ = 0;
        vbo_usage_flags_ = ibo_usage_flags_ = 0;
    }

    // ---------- create ----------
    // Fill the per-LOD index sub-ranges on a freshly-built GPU record from the
    // create-info's LOD counts (LOD0 first). Shared by create() + allocateOnly().
    static void fillLodRanges(MeshGpuRecord& gpu, const MeshCreateInfo& ci,
                              uint32_t index_size)
    {
        const uint32_t global0 =
            static_cast<uint32_t>(gpu.index_buffer_range.offset / index_size);
        if (ci.lod_index_counts.empty()) {
            gpu.lod_count          = 1;
            gpu.lod_index_first[0] = global0;
            gpu.lod_index_count[0] = gpu.index_count;
            return;
        }
        uint32_t n = static_cast<uint32_t>(ci.lod_index_counts.size());
        if (n > kMaxMeshLod) n = kMaxMeshLod;
        uint32_t cum = 0;
        for (uint32_t i = 0; i < n; ++i) {
            gpu.lod_index_first[i] = global0 + cum;
            gpu.lod_index_count[i] = ci.lod_index_counts[i];
            cum += ci.lod_index_counts[i];
        }
        gpu.lod_count = static_cast<uint8_t>(n);
    }

    Expected<MeshHandle>
    MeshResources::create(const MeshCreateInfo& ci)
    {
        if (ci.layout_id == invalid_layout_id || ci.vertex_stride == 0)
            return lux::cxx::unexpected(make_error_code(ERenderError::InvalidArgument));

        // Enforce the hard cap: with no free slot, a new mesh would push_back-grow
        // cpu_records_/gpu_records_/gens_, reallocating storage the game thread
        // reads concurrently (picking) — the exact UAF init()'s reserve() exists to
        // prevent. Reject instead of growing past the cap. (C-4)
        if (free_.empty() && cpu_records_.size() >= mesh_max_count_)
            return lux::cxx::unexpected(make_error_code(ERenderError::OutOfMemory));

        // 1) Infer index_type (original logic unchanged)
        EIndexType it = ci.index_type;
        if (it == EIndexType::None) {
            if (!ci.index_buffer.empty()) {
                const auto bytes = ci.index_buffer.size_bytes();
                if (bytes % 4 == 0)      it = EIndexType::UInt32;
                else if (bytes % 2 == 0) it = EIndexType::UInt16;
                else return lux::cxx::unexpected(make_error_code(ERenderError::InvalidArgument));
            }
            else {
                it = EIndexType::UInt32;
            }
        }

        // 2) VBO / IBO sub-allocation + write (DESIGN-04: now uses ChainedArenaAllocator)
        // VBO alignment must be a multiple of vertex_stride so that
        // (offset / stride) yields an exact integer for base_vertex.
        const uint64_t vbo_align = std::lcm(uint64_t(256), uint64_t(ci.vertex_stride));
        auto vrExp = suballoc(vbo_arena_, ci.vertex_buffer, vbo_align);
        if (!vrExp) return lux::cxx::unexpected(vrExp.error());
        auto irExp = suballoc(ibo_arena_, ci.index_buffer);
        if (!irExp) return lux::cxx::unexpected(irExp.error());

        const uint32_t index_size = (it == EIndexType::UInt16 ? 2u : 4u);

        // 3) Build CPU and GPU records
        MeshCpuRecord cpu{};
        cpu.local_bounds = ci.bounds.value_or(math::AABB{});
        cpu.valid        = true;

        MeshGpuRecord gpu{};
        gpu.layout_id           = ci.layout_id;
        gpu.vertex_stride       = ci.vertex_stride;
        gpu.vertex_buffer_range = vrExp->range;
        gpu.index_buffer_range  = irExp->range;
        gpu.vbo_segment         = vrExp->segment;
        gpu.ibo_segment         = irExp->segment;
        gpu.vbo_alloc_handle    = vrExp->alloc_handle;
        gpu.ibo_alloc_handle    = irExp->alloc_handle;
        gpu.index_type          = it;
        gpu.index_count         = static_cast<uint32_t>(ci.index_buffer.size_bytes() / index_size);
        gpu.ready.store(false, std::memory_order_relaxed);
        fillLodRanges(gpu, ci, index_size);

        // 4) Segment table: treat the entire index_buffer as one segment
        MeshInfoGpu seg{};

        const uint32_t index_first =
            calcIndexStartGlobal(it, gpu.index_buffer_range, /*sub_first=*/0);
        const uint32_t index_count =
            ci.index_buffer.empty()
            ? 0u
            : static_cast<uint32_t>(ci.index_buffer.size_bytes() / index_size);

        const uint32_t base_vertex =
            static_cast<uint32_t>(gpu.vertex_buffer_range.offset / gpu.vertex_stride);

        seg.index_first = index_first;
        seg.index_count = index_count;
        seg.base_vertex = static_cast<int32_t>(base_vertex);

        if (ci.bounds.has_value()) {
            std::memcpy(seg.bounds_min, ci.bounds->min.data(), sizeof(float) * 3);
            std::memcpy(seg.bounds_max, ci.bounds->max.data(), sizeof(float) * 3);
        }
        else {
            seg.bounds_min[0] = seg.bounds_min[1] = seg.bounds_min[2] = +FLT_MAX;
            seg.bounds_max[0] = seg.bounds_max[1] = seg.bounds_max[2] = -FLT_MAX;
        }

        auto sh = segments_ssbo_.add(seg);
        gpu.segment_slot = sh;

        // 5) Handle allocation — slot reuse from free list or new push_back
        uint32_t id;
        if (!free_.empty()) {
            id = free_.back();
            free_.pop_back();
            cpu_records_[id] = std::move(cpu);
            gpu_records_[id] = std::move(gpu);
            // slot_count_ already covers this index — no update needed.
        }
        else {
            id = static_cast<uint32_t>(cpu_records_.size());
            cpu_records_.push_back(std::move(cpu));
            gpu_records_.push_back(std::move(gpu));
            gens_.push_back(1);
            // Publish the new slot to the render thread.  This store(release)
            // acts as a fence: all writes to cpu_records_[id], gpu_records_[id]
            // and gens_[id] above are guaranteed visible to any thread that
            // subsequently executes slot_count_.load(acquire) and sees id+1.
            slot_count_.store(static_cast<uint32_t>(cpu_records_.size()),
                              std::memory_order_release);
        }

        // Enqueue the CPU→GPU copy now that we know the mesh index.
        enqueueStagingTransfer(ci.vertex_buffer, vrExp->range.offset, vrExp->segment,
                               ci.index_buffer, irExp->range.offset, irExp->segment, id);

        return MeshHandle{ id, gens_[id] };
    }

    // ---------- allocateOnly (async path — no staging transfer) ----------
    Expected<MeshAllocResult>
    MeshResources::allocateOnly(const MeshCreateInfo& ci)
    {
        if (ci.layout_id == invalid_layout_id || ci.vertex_stride == 0)
            return lux::cxx::unexpected(make_error_code(ERenderError::InvalidArgument));

        // Enforce the hard cap (see create()): reject rather than reallocate the
        // game-thread-read vectors. (C-4)
        if (free_.empty() && cpu_records_.size() >= mesh_max_count_)
            return lux::cxx::unexpected(make_error_code(ERenderError::OutOfMemory));

        // 1) Infer index_type
        EIndexType it = ci.index_type;
        if (it == EIndexType::None) {
            if (!ci.index_buffer.empty()) {
                const auto bytes = ci.index_buffer.size_bytes();
                if (bytes % 4 == 0)      it = EIndexType::UInt32;
                else if (bytes % 2 == 0) it = EIndexType::UInt16;
                else return lux::cxx::unexpected(make_error_code(ERenderError::InvalidArgument));
            }
            else {
                it = EIndexType::UInt32;
            }
        }

        // 2) VBO / IBO sub-allocation (no data copy — worker will do it)
        const uint64_t vbo_align = std::lcm(uint64_t(256), uint64_t(ci.vertex_stride));
        auto vrExp = suballoc(vbo_arena_, ci.vertex_buffer, vbo_align);
        if (!vrExp) return lux::cxx::unexpected(vrExp.error());
        auto irExp = suballoc(ibo_arena_, ci.index_buffer);
        if (!irExp) return lux::cxx::unexpected(irExp.error());

        const uint32_t index_size = (it == EIndexType::UInt16 ? 2u : 4u);

        // 3) Build CPU and GPU records (ready = false)
        MeshCpuRecord cpu{};
        cpu.local_bounds = ci.bounds.value_or(math::AABB{});
        cpu.valid        = true;

        MeshGpuRecord gpu{};
        gpu.layout_id           = ci.layout_id;
        gpu.vertex_stride       = ci.vertex_stride;
        gpu.vertex_buffer_range = vrExp->range;
        gpu.index_buffer_range  = irExp->range;
        gpu.vbo_segment         = vrExp->segment;
        gpu.ibo_segment         = irExp->segment;
        gpu.vbo_alloc_handle    = vrExp->alloc_handle;
        gpu.ibo_alloc_handle    = irExp->alloc_handle;
        gpu.index_type          = it;
        gpu.index_count         = static_cast<uint32_t>(ci.index_buffer.size_bytes() / index_size);
        gpu.ready.store(false, std::memory_order_relaxed);
        fillLodRanges(gpu, ci, index_size);

        // 4) Segment table — write index_count=0 so GPU skips non-ready meshes
        MeshInfoGpu seg{};
        seg.index_first = calcIndexStartGlobal(it, gpu.index_buffer_range, 0);
        seg.index_count = 0;  // deferred until markReady()
        seg.base_vertex = static_cast<int32_t>(
            gpu.vertex_buffer_range.offset / gpu.vertex_stride);

        if (ci.bounds.has_value()) {
            std::memcpy(seg.bounds_min, ci.bounds->min.data(), sizeof(float) * 3);
            std::memcpy(seg.bounds_max, ci.bounds->max.data(), sizeof(float) * 3);
        } else {
            seg.bounds_min[0] = seg.bounds_min[1] = seg.bounds_min[2] = +FLT_MAX;
            seg.bounds_max[0] = seg.bounds_max[1] = seg.bounds_max[2] = -FLT_MAX;
        }

        auto seg_sh = segments_ssbo_.add(seg);
        gpu.segment_slot = seg_sh;

        // 5) Handle allocation — slot reuse from free list or new push_back
        uint32_t id;
        if (!free_.empty()) {
            id = free_.back();
            free_.pop_back();
            cpu_records_[id] = std::move(cpu);
            gpu_records_[id] = std::move(gpu);
        } else {
            id = static_cast<uint32_t>(cpu_records_.size());
            cpu_records_.push_back(std::move(cpu));
            gpu_records_.push_back(std::move(gpu));
            gens_.push_back(1);
            slot_count_.store(static_cast<uint32_t>(cpu_records_.size()),
                              std::memory_order_release);
        }

        // No enqueueStagingTransfer — the worker thread will copy data directly.
        return MeshAllocResult{ MeshHandle{ id, gens_[id] }, vrExp->range, irExp->range,
                                vrExp->segment, irExp->segment };
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

        gpu.ready.store(true, std::memory_order_release);
    }

    bool MeshResources::destroy(MeshHandle h)
    {
        if (!alive(h)) return false;
        auto& gpu = gpu_records_[h.index];
        // DESIGN-04: Free arena space (VBO/IBO) so it can be reused — but DEFER it
        // by frames-in-flight. Returning the range immediately lets a destroy-then-
        // create within ~2 frames re-allocate the identical bytes, and the async
        // transfer-queue write into them races the still-executing graphics frames
        // (N-1/N-2) that draw the destroyed mesh's old vertices. Retire into the
        // per-FIF ring; retireFrameStagingBuffers() returns them to the arena after
        // the slot fence is waited.
        if (gpu.vertex_buffer_range.isValid())
            retired_ranges_[current_fi_].push_back(RetiredArenaRange{
                true, gpu.vbo_segment, gpu.vertex_buffer_range, gpu.vbo_alloc_handle });
        if (gpu.index_buffer_range.isValid())
            retired_ranges_[current_fi_].push_back(RetiredArenaRange{
                false, gpu.ibo_segment, gpu.index_buffer_range, gpu.ibo_alloc_handle });
        cpu_records_[h.index].valid = false;
        gens_[h.index]++;
        free_.push_back(h.index);
        return true;
    }

    bool MeshResources::alive(MeshHandle h) const
    {
        // Use slot_count_ (not cpu_records_.size()) to avoid a data race:
        // the game thread writes the vector's internal size pointer in push_back
        // while the render thread could be reading .size().
        // The release/acquire pair on slot_count_ ensures all writes to
        // cpu_records_[h.index], gpu_records_[h.index] and gens_[h.index]
        // are visible to the render thread once it sees the new count.
        return h.valid() &&
            h.index < slot_count_.load(std::memory_order_acquire) &&
            cpu_records_[h.index].valid &&
            gens_[h.index] == h.gen;
    }

    Expected<VertexLayoutId>
        MeshResources::layoutId(MeshHandle h) const
    {
        if (!alive(h)) return lux::cxx::unexpected(make_error_code(ERenderError::NotFound));
        return gpu_records_[h.index].layout_id;
    }

    // DESIGN-04: Free-list based sub-allocation (replaces bump cursor)
    Expected<MeshResources::SegmentedRange>
        MeshResources::suballoc(ChainedArenaAllocator& arena, std::span<const std::byte> data, uint64_t alignment)
    {
        if (data.empty()) return SegmentedRange{};
        const uint64_t need = data.size_bytes();
        auto alloc = arena.allocate(need, alignment);
        if (!alloc.valid())
        {
            // Auto-grow: add a new buffer segment and retry.
            const bool is_vbo = (&arena == &vbo_arena_);
            const VkDeviceSize seg_cap = is_vbo ? vbo_segment_cap_ : ibo_segment_cap_;
            const VkDeviceSize grow_bytes = std::max(seg_cap, static_cast<VkDeviceSize>(need));

            VRAMBudgetGuard budget(device_ctx_->vmaAllocator());
            if (!budget.canAllocate(grow_bytes))
                return lux::cxx::unexpected(make_error_code(ERenderError::OutOfMemory));

            auto& bufs       = is_vbo ? vbo_buffers_ : ibo_buffers_;
            auto& allocs_vec = is_vbo ? vbo_allocs_  : ibo_allocs_;
            const VkBufferUsageFlags usage = is_vbo ? vbo_usage_flags_ : ibo_usage_flags_;

            if (!addBufferSegment(grow_bytes, usage, bufs, allocs_vec, arena))
                return lux::cxx::unexpected(make_error_code(ERenderError::OutOfMemory));

            alloc = arena.allocate(need, alignment);
            if (!alloc.valid())
                return lux::cxx::unexpected(make_error_code(ERenderError::OutOfMemory));
        }
        return SegmentedRange{ BufferRange{ alloc.offset, alloc.size }, alloc.segment_index, alloc.handle };
    }

    bool MeshResources::createArena(VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer& out, VmaAllocation& out_alloc)
    {
        auto vma = device_ctx_->vmaAllocator();

        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = bytes;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bi.queueFamilyIndexCount = 0;
        bi.pQueueFamilyIndices = nullptr;

        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.flags = 0;
        ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        VkResult result = vmaCreateBuffer(vma, &bi, &ai, &out, &out_alloc, nullptr);
        return result == VK_SUCCESS;
    }

    bool MeshResources::addBufferSegment(VkDeviceSize bytes, VkBufferUsageFlags usage,
                                          std::vector<VkBuffer>& bufs, std::vector<VmaAllocation>& allocs,
                                          ChainedArenaAllocator& arena)
    {
        VkBuffer buf; VmaAllocation alloc;
        if (!createArena(bytes, usage, buf, alloc))
            return false;
        bufs.push_back(buf);
        allocs.push_back(alloc);
        auto idx = arena.addSegment(bytes);
        return idx != ChainedArenaAllocator::kInvalidSegment;
    }

    void MeshResources::enqueueStagingTransfer(
        std::span<const std::byte> vbo_data, VkDeviceSize vbo_off,
        uint16_t vbo_seg,
        std::span<const std::byte> ibo_data, VkDeviceSize ibo_off,
        uint16_t ibo_seg,
        uint32_t mesh_index)
    {
        if (vbo_data.empty() && ibo_data.empty()) return;

        auto vma = device_ctx_->vmaAllocator();

        // One staging buffer covering both VBO and IBO data.
        const VkDeviceSize vbo_bytes = vbo_data.size_bytes();
        const VkDeviceSize ibo_bytes = ibo_data.size_bytes();
        const VkDeviceSize total     = vbo_bytes + ibo_bytes;
        if (total == 0) return;

        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size  = total;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info;
        VkBuffer      stg_buf;
        VmaAllocation stg_alloc;
        VkResult result = vmaCreateBuffer(vma, &bi, &ai, &stg_buf, &stg_alloc, &info);
        if (result != VK_SUCCESS)
            return;   // staging allocation failed — skip this transfer

        auto* p = static_cast<char*>(info.pMappedData);
        if (vbo_bytes) std::memcpy(p,             vbo_data.data(), vbo_bytes);
        if (ibo_bytes) std::memcpy(p + vbo_bytes, ibo_data.data(), ibo_bytes);

        // ONE entry carrying BOTH regions (VBO at staging offset 0, IBO at
        // vbo_bytes). Atomic: a full SPSC ring can no longer accept the VBO half
        // and drop the IBO half (which used to mark the mesh ready with a garbage
        // index buffer). On a full ring, destroy the staging buffer and leave the
        // mesh un-ready (invisible) rather than leaking + drawing garbage.
        PendingTransfer t{};
        t.stg_buf        = stg_buf;
        t.stg_alloc      = stg_alloc;
        t.vbo_dst_buf    = vbo_bytes ? vbo_buffers_[vbo_seg] : VK_NULL_HANDLE;
        t.vbo_dst_offset = vbo_off;
        t.vbo_size       = vbo_bytes;
        t.ibo_dst_buf    = ibo_bytes ? ibo_buffers_[ibo_seg] : VK_NULL_HANDLE;
        t.ibo_dst_offset = ibo_off;
        t.ibo_size       = ibo_bytes;
        t.mesh_index     = mesh_index;
        if (!pending_queue_.emplace(t))
        {
            // Ring full: do not leak the staging buffer. The mesh stays un-ready;
            // the caller's deferred reply will report it (better than corruption).
            vmaDestroyBuffer(vma, stg_buf, stg_alloc);
        }
    }

    // Render-thread: record copies + barrier, mark meshes ready, schedule
    // staging-buffer deletion.
    void MeshResources::flushPendingTransfers(VkCommandBuffer cmd, uint32_t fi)
    {
        bool        had_transfers = false;
        PendingTransfer t;

        // Collect mesh indices that completed transfer — mark ready after barrier.
        std::vector<uint32_t> completed_indices;

        while (pending_queue_.pop(t)) {
            // VBO region at staging offset 0, IBO region at offset vbo_size.
            if (t.vbo_size > 0) {
                VkBufferCopy region{};
                region.srcOffset = 0;
                region.dstOffset = t.vbo_dst_offset;
                region.size      = t.vbo_size;
                vkCmdCopyBuffer(cmd, t.stg_buf, t.vbo_dst_buf, 1, &region);
            }
            if (t.ibo_size > 0) {
                VkBufferCopy region{};
                region.srcOffset = t.vbo_size;   // IBO data follows the VBO in staging
                region.dstOffset = t.ibo_dst_offset;
                region.size      = t.ibo_size;
                vkCmdCopyBuffer(cmd, t.stg_buf, t.ibo_dst_buf, 1, &region);
            }

            completed_indices.push_back(t.mesh_index);

            // One staging buffer per entry — retire it after the frame fence.
            if (t.stg_alloc != nullptr) {
                deferred_deletions_[fi].emplace_back(device_ctx_->vmaAllocator(), t.stg_buf, t.stg_alloc);
            }

            had_transfers = true;
        }

        if (had_transfers) {
            // Barrier: TRANSFER_WRITE → VERTEX_INPUT / INDEX_INPUT
            VkMemoryBarrier2 mem_barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mem_barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
            mem_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mem_barrier.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                                        VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
            mem_barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                                        VK_ACCESS_2_INDEX_READ_BIT;

            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers    = &mem_barrier;
            vkCmdPipelineBarrier2(cmd, &dep);

            // Mark ready only after the barrier has been recorded — ensures
            // any draw using these meshes sees the completed transfer.
            for (uint32_t idx : completed_indices)
                gpu_records_[idx].ready.store(true, std::memory_order_release);
        }
    }

    // Render-thread: called in beginFrame() after waiting on the frame fence.
    void MeshResources::retireFrameStagingBuffers(uint32_t fi)
    {
        auto& slot = deferred_deletions_[fi];
        slot.clear();

        // The slot's fence has been waited (called from onBeginFrame), so any
        // frame that referenced these arena ranges is complete — return them to
        // the arena now, safe to re-allocate. (#23)
        auto& ranges = retired_ranges_[fi];
        for (const auto& r : ranges)
        {
            if (!r.range.isValid())
                continue;
            if (r.is_vbo)
                vbo_arena_.free({ r.segment, r.range.offset, r.range.size, r.handle });
            else
                ibo_arena_.free({ r.segment, r.range.offset, r.range.size, r.handle });
        }
        ranges.clear();
    }

    // ---------- Async acquire barriers ----------

    void MeshResources::pushAcquireBarrier(
        VkBuffer buf, VkDeviceSize offset, VkDeviceSize size,
        uint32_t src_queue_family, uint32_t dst_queue_family)
    {
        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask       = VK_PIPELINE_STAGE_2_NONE;
        b.srcAccessMask      = VK_ACCESS_2_NONE;
        b.dstStageMask       = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        b.dstAccessMask      = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
        b.srcQueueFamilyIndex = src_queue_family;
        b.dstQueueFamilyIndex = dst_queue_family;
        b.buffer             = buf;
        b.offset             = offset;
        b.size               = size;
        pending_acquire_barriers_.push_back(b);
    }

    void MeshResources::recordAcquireBarriers(VkCommandBuffer cmd)
    {
        if (pending_acquire_barriers_.empty()) return;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount = static_cast<uint32_t>(pending_acquire_barriers_.size());
        dep.pBufferMemoryBarriers    = pending_acquire_barriers_.data();
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
        if (pending_staging_copies_.empty()) return;

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
        bar.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        bar.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        bar.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers    = &bar;
        vkCmdPipelineBarrier2(cmd, &dep);

        for (auto& c : pending_staging_copies_)
            markReady(c.mesh_index);
        pending_staging_copies_.clear();
    }

    // ── TransferScheduler integration ──

    void MeshResources::submitTransfers(TransferScheduler& scheduler)
    {
        // 1. Drain SPSC pending transfers → submit buffer copies.
        {
            PendingTransfer t;
            while (pending_queue_.pop(t)) {
                // One entry → both copies (VBO at staging offset 0, IBO at vbo_size).
                if (t.vbo_size > 0)
                    scheduler.submitBufferCopy({
                        .src        = t.stg_buf,
                        .src_offset = 0,
                        .dst        = t.vbo_dst_buf,
                        .dst_offset = t.vbo_dst_offset,
                        .size       = t.vbo_size,
                        .domain     = EBufferDomain::VertexInput,
                    });
                if (t.ibo_size > 0)
                    scheduler.submitBufferCopy({
                        .src        = t.stg_buf,
                        .src_offset = t.vbo_size,
                        .dst        = t.ibo_dst_buf,
                        .dst_offset = t.ibo_dst_offset,
                        .size       = t.ibo_size,
                        .domain     = EBufferDomain::VertexInput,
                    });

                // Schedule staging buffer for deferred deletion.
                if (t.stg_alloc != nullptr)
                    deferred_deletions_[current_fi_].emplace_back(
                        device_ctx_->vmaAllocator(), t.stg_buf, t.stg_alloc);

                markReady(t.mesh_index);
            }
        }

        // 2. StagingOnly fallback copies.
        for (auto& c : pending_staging_copies_)
        {
            if (c.vbo_size > 0)
            {
                scheduler.submitBufferCopy({
                    .src        = c.stg_buf,
                    .src_offset = c.vbo_stg_offset,
                    .dst        = c.vbo_dst,
                    .dst_offset = c.vbo_dst_offset,
                    .size       = c.vbo_size,
                    .domain     = EBufferDomain::VertexInput,
                });
            }
            if (c.ibo_size > 0)
            {
                scheduler.submitBufferCopy({
                    .src        = c.stg_buf,
                    .src_offset = c.ibo_stg_offset,
                    .dst        = c.ibo_dst,
                    .dst_offset = c.ibo_dst_offset,
                    .size       = c.ibo_size,
                    .domain     = EBufferDomain::VertexInput,
                });
            }
            markReady(c.mesh_index);
        }
        pending_staging_copies_.clear();

        // 3. QFOT acquire barriers.
        for (auto& b : pending_acquire_barriers_)
        {
            scheduler.submitQFOTAcquire({
                .kind       = QFOTAcquireRequest::Kind::Buffer,
                .buffer     = b.buffer,
                .buf_offset = b.offset,
                .buf_size   = b.size,
                .src_family = b.srcQueueFamilyIndex,
                .dst_family = b.dstQueueFamilyIndex,
                .domain     = EBufferDomain::VertexInput,
            });
        }
        pending_acquire_barriers_.clear();
    }

    // ---------- Defragmentation ----------

    uint32_t MeshResources::maybeDefragment(VkCommandBuffer cmd, float threshold)
    {
        uint32_t total_ops = 0;

        // Helper lambda: defrag one arena type (VBO or IBO).
        // is_vbo selects which gpu_record fields to update.
        auto defragArena = [&](ChainedArenaAllocator& arena,
                               const std::vector<VkBuffer>& bufs,
                               bool is_vbo) -> uint32_t
        {
            uint32_t ops = 0;
            const uint16_t seg_count = arena.segmentCount();

            for (uint16_t seg = 0; seg < seg_count; ++seg)
            {
                auto* a = arena.segment(seg);
                if (!a || a->fragmentationRatio() <= threshold)
                    continue;

                auto copy_ops = a->planDefragmentation();
                if (copy_ops.empty()) continue;

                VkBuffer buf = bufs[seg];

                // 1. Record GPU copy commands (within same buffer).
                for (const auto& op : copy_ops)
                {
                    VkBufferCopy region{};
                    region.srcOffset = op.src_offset;
                    region.dstOffset = op.dst_offset;
                    region.size      = op.size;
                    vkCmdCopyBuffer(cmd, buf, buf, 1, &region);
                }

                // 2. Update gpu_records_ offsets for affected meshes.
                const uint32_t count = slot_count_.load(std::memory_order_acquire);
                for (uint32_t i = 0; i < count; ++i)
                {
                    auto& gpu = gpu_records_[i];
                    if (!gpu.ready.load(std::memory_order_relaxed)) continue;

                    BufferRange& br = is_vbo ? gpu.vertex_buffer_range : gpu.index_buffer_range;
                    const uint16_t rec_seg = is_vbo ? gpu.vbo_segment : gpu.ibo_segment;
                    if (rec_seg != seg || !br.isValid()) continue;

                    // Find which CopyOp covers this allocation's offset.
                    for (const auto& op : copy_ops)
                    {
                        if (br.offset >= op.src_offset &&
                            br.offset + br.size <= op.src_offset + op.size)
                        {
                            const VkDeviceSize delta = br.offset - op.src_offset;
                            br.offset = op.dst_offset + delta;

                            // Update segment SSBO entry.
                            if (auto* seg_entry = segments_ssbo_.get(gpu.segment_slot))
                            {
                                if (is_vbo)
                                    seg_entry->base_vertex = static_cast<int32_t>(
                                        gpu.vertex_buffer_range.offset / gpu.vertex_stride);
                                else
                                    seg_entry->index_first = calcIndexStartGlobal(
                                        gpu.index_type, gpu.index_buffer_range, 0);
                                segments_ssbo_.touch(gpu.segment_slot);
                            }
                            break;
                        }
                    }
                }

                // 3. Update arena bookkeeping.
                a->applyDefragmentation(copy_ops);

                // 4. Refresh VMA alloc handles in gpu_records_ — the old
                //    handles were invalidated by vmaClearVirtualBlock inside
                //    applyDefragmentation().  Look up new handles by offset.
                for (uint32_t i = 0; i < count; ++i)
                {
                    auto& gpu = gpu_records_[i];
                    if (!gpu.ready.load(std::memory_order_relaxed)) continue;
                    const uint16_t rec_seg = is_vbo ? gpu.vbo_segment : gpu.ibo_segment;
                    if (rec_seg != seg) continue;
                    const BufferRange& br = is_vbo ? gpu.vertex_buffer_range : gpu.index_buffer_range;
                    if (!br.isValid()) continue;
                    auto h = a->findHandleAt(br.offset);
                    if (is_vbo)
                        gpu.vbo_alloc_handle = h;
                    else
                        gpu.ibo_alloc_handle = h;
                }

                ops += static_cast<uint32_t>(copy_ops.size());
            }
            return ops;
        };

        total_ops += defragArena(vbo_arena_, vbo_buffers_, /*is_vbo=*/true);
        total_ops += defragArena(ibo_arena_, ibo_buffers_, /*is_vbo=*/false);

        // Barrier if we recorded any copies.
        if (total_ops > 0)
        {
            VkMemoryBarrier2 bar{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            bar.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
            bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            bar.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                                VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
            bar.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                                VK_ACCESS_2_INDEX_READ_BIT;
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers    = &bar;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        return total_ops;
    }

} // namespace lux::render
