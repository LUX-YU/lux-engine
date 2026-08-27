#include <lux/engine/render/scene/SpatialCullGrid.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp> // create/destroyGpuBufferVmaBuffer
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp> // DeviceContext::vmaAllocator()

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lux::render
{
    // =========================================================================
    //  Lifecycle
    // =========================================================================
    SpatialCullGrid::~SpatialCullGrid()
    {
        if (initialized_)
            shutdown();
    }

    void SpatialCullGrid::init(const InitInfo& info)
    {
        if (initialized_)
            return; // idempotent

        device_ctx_ = info.device_context;
        deferred_queue_ = info.deferred_queue;
        cell_size_ = (info.cell_size > 0.0f) ? info.cell_size : 128.0f;
        cull_distance_ = (info.cull_distance > 0.0f) ? info.cull_distance : 512.0f;

        const uint32_t ring = std::max(info.frames_in_flight, 1u) + 1u;
        ring_buffers_.assign(ring, VK_NULL_HANDLE);
        ring_allocs_.assign(ring, nullptr);
        ring_mapped_.assign(ring, nullptr);
        ring_sizes_.assign(ring, VkDeviceSize{0});
        ring_cursor_ = 0;
        current_slot_ = 0;
        last_upload_serial_ = ~0ull;

        // Pre-allocate every ring slot and fill it with all-active (0xFF) so
        // activeMaskBuffer() returns a valid, never-over-culling buffer even before
        // the first update() (which in practice always runs before cull anyway).
        const VkDeviceSize init_size =
            static_cast<VkDeviceSize>(std::max(info.initial_capacity, 1u)) * sizeof(uint32_t);
        if (device_ctx_ != nullptr)
        {
            for (uint32_t i = 0; i < ring; ++i)
            {
                VkBuffer buf{VK_NULL_HANDLE};
                VmaAllocation alloc{nullptr};
                void* mapped{nullptr};
                if (createGpuBufferVmaBuffer(
                        device_ctx_->vmaAllocator(),
                        init_size,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        /*cpu_writable=*/true,
                        &buf,
                        &alloc,
                        &mapped) &&
                    buf != VK_NULL_HANDLE && mapped != nullptr)
                {
                    std::memset(mapped, 0xFF, static_cast<std::size_t>(init_size));
                    ring_buffers_[i] = buf;
                    ring_allocs_[i] = alloc;
                    ring_mapped_[i] = mapped;
                    ring_sizes_[i] = init_size;
                }
            }
        }

        initialized_ = true;
    }

    void SpatialCullGrid::shutdown()
    {
        if (!initialized_)
            return;
        destroyRing();
        mask_.clear();
        mask_.shrink_to_fit();
        cell_active_.clear();
        mask_slot_count_ = 0;
        device_ctx_ = nullptr;
        deferred_queue_ = nullptr;
        initialized_ = false;
    }

    void SpatialCullGrid::destroyRing() noexcept
    {
        if (device_ctx_ != nullptr)
        {
            // shutdown runs on the scene-teardown path (device idle), so an inline
            // destroy is safe — no in-flight frame can still be reading these.
            for (std::size_t i = 0; i < ring_buffers_.size(); ++i)
            {
                destroyGpuBufferVmaBuffer(device_ctx_->vmaAllocator(), ring_buffers_[i], ring_allocs_[i]);
            }
        }
        ring_buffers_.clear();
        ring_allocs_.clear();
        ring_mapped_.clear();
        ring_sizes_.clear();
    }

    void SpatialCullGrid::setCellSize(float s) noexcept
    {
        if (s > 0.0f)
            cell_size_ = s;
    }

    void SpatialCullGrid::setCullDistance(float r) noexcept
    {
        if (r > 0.0f)
            cull_distance_ = r;
    }

    // =========================================================================
    //  Pure cell math (no GPU / no state — CPU-unit-testable)
    // =========================================================================
    void
    SpatialCullGrid::cellCoord(float world_x, float world_y, float cell_size, int32_t& out_cx, int32_t& out_cy) noexcept
    {
        const float inv = 1.0f / cell_size;
        out_cx = static_cast<int32_t>(std::floor(world_x * inv));
        out_cy = static_cast<int32_t>(std::floor(world_y * inv));
    }

    bool SpatialCullGrid::cellInRange(
        int32_t cx,
        int32_t cy,
        float cell_size,
        float cull_distance,
        std::span<const std::array<float, 3>> cameras
    ) noexcept
    {
        if (cameras.empty())
            return true; // no cull source → keep active (never over-cull)
        const float ccx = (static_cast<float>(cx) + 0.5f) * cell_size;
        const float ccz = (static_cast<float>(cy) + 0.5f) * cell_size;
        const float eff = cull_distance + cell_size; // one-cell slack
        const float eff2 = eff * eff;
        for (const auto& cam : cameras)
        {
            // Ground-plane X-Z distance (Y-up: the height axis is spanned, NOT
            // used). Camera is {x,y,z}; use x + z. (Using cam[1]/Y here ignored
            // depth → cells loaded as a strip down the view axis.)
            const float dx = ccx - cam[0];
            const float dz = ccz - cam[2];
            if (dx * dx + dz * dz < eff2)
                return true;
        }
        return false;
    }

    // =========================================================================
    //  Per-frame update
    // =========================================================================
    void SpatialCullGrid::update(
        uint64_t serial,
        std::span<const std::array<float, 3>> cameras,
        const InstanceResources& instances
    )
    {
        if (!initialized_)
            return;

        // Extract each alive instance's (slot, world bsphere X + Z) — the ONLY place
        // that touches InstanceResources. Y-up engine → stream on the X-Z ground
        // plane (bsphere[0]=x, bsphere[2]=z; bsphere[1]=Y/height is spanned).
        // Tombstones (bsphere.w<0) are skipped (they already cull-early-out). Then
        // delegate to the InstanceResources-free core below.
        extract_scratch_.clear();
        const uint32_t slot_count = instances.slotCount();
        const float page_size = instances.spatialTileSize();
        const auto alive = instances.denseAliveSlots();
        extract_scratch_.reserve(alive.size());
        for (const uint32_t slot : alive)
        {
            if (slot >= slot_count)
                continue;
            const auto& cull = instances.cullMetaAt(InstanceSlot{slot});
            if (cull.bsphere[3] < 0.0f)
                continue;
            // SpatialCullGrid is a temporary coarse-mask compatibility path;
            // reconstruct only at its 128m-cell boundary. Fine view/HZB/shadow
            // culling retains exact page/local coordinates on the GPU.
            extract_scratch_.push_back(InstanceXY{
                slot,
                static_cast<float>(cull.bsphere_page[0]) * page_size + cull.bsphere[0],
                static_cast<float>(cull.bsphere_page[2]) * page_size + cull.bsphere[2]}
            );
        }
        update(serial, cameras, extract_scratch_, slot_count);
    }

    void SpatialCullGrid::update(
        uint64_t serial,
        std::span<const std::array<float, 3>> cameras,
        std::span<const InstanceXY> alive,
        uint32_t slot_count
    )
    {
        if (!initialized_)
            return;

        // serial de-dup: multiple cull passes of one frame (Forward / Deferred /
        // Shadow) share ONE ring slot + ONE mask. The first call this frame advances
        // the ring and recomputes; later calls are no-ops (current_slot_ stays put).
        if (serial == last_upload_serial_)
            return;
        last_upload_serial_ = serial;

        // Advance the ring once per frame. A reused slot was last written `ring`
        // frames ago (>= FIF+1) → GPU-idle, so writing it can't tear an in-flight frame.
        if (!ring_buffers_.empty())
            ring_cursor_ = (ring_cursor_ + 1u) % static_cast<uint32_t>(ring_buffers_.size());
        current_slot_ = ring_cursor_;

        recomputeMask(cameras, alive, slot_count);
        uploadMask();
    }

    void SpatialCullGrid::recomputeMask(
        std::span<const std::array<float, 3>> cameras,
        std::span<const InstanceXY> alive,
        uint32_t slot_count
    )
    {
        mask_.assign(slot_count, 1u); // default active — never over-cull an unclassified slot
        mask_slot_count_ = slot_count;

        stat_total_instances_ = static_cast<uint32_t>(alive.size());
        stat_active_instances_ = 0;
        stat_active_cells_ = 0;
        stat_total_cells_ = 0;

        // Disabled or no cull source → everything active (equivalent to the
        // pre-partition behaviour; lets the editor toggle the coarse cull off).
        if (!enabled_ || cameras.empty())
        {
            stat_active_instances_ = stat_total_instances_;
            return;
        }

        cell_active_.clear();

        // Cell-active determination goes through the pure function cellInRange
        // (the same logic is covered by CPU unit tests); this just adds a
        // per-cell cache on top, so multiple instances in the same cell don't
        // redundantly recompute the distance to the cameras.
        auto cellActive = [&](CellKey key) -> bool {
            const auto it = cell_active_.find(key);
            if (it != cell_active_.end())
                return it->second != 0u;
            const bool active = cellInRange(key.x, key.y, cell_size_, cull_distance_, cameras);
            cell_active_.emplace(key, active ? 1u : 0u);
            return active;
        };

        for (const auto& inst : alive)
        {
            if (inst.slot >= slot_count)
                continue;
            CellKey key{};
            cellCoord(inst.x, inst.y, cell_size_, key.x, key.y);
            if (cellActive(key))
                ++stat_active_instances_;
            else
                mask_[inst.slot] = 0u; // dormant cell → masked out of every GPU cull dispatch
        }

        stat_total_cells_ = static_cast<uint32_t>(cell_active_.size());
        for (const auto& [k, v] : cell_active_)
        {
            (void)k;
            if (v)
                ++stat_active_cells_;
        }
    }

    void SpatialCullGrid::uploadMask()
    {
        if (ring_buffers_.empty())
            return;
        const uint32_t slot = current_slot_;
        const VkDeviceSize required = static_cast<VkDeviceSize>(std::max(mask_slot_count_, 1u)) * sizeof(uint32_t);

        // Grow this slot's buffer if the instance count outgrew it. The slot is
        // GPU-idle (last written `ring` frames ago) but still route the free through
        // the deferred queue for safety. Over-allocate 2x to amortise growth.
        if (required > ring_sizes_[slot])
        {
            if (ring_buffers_[slot] != VK_NULL_HANDLE)
            {
                if (deferred_queue_ != nullptr)
                    deferred_queue_->retireBuffer(ring_buffers_[slot], ring_allocs_[slot]);
                else
                    destroyGpuBufferVmaBuffer(device_ctx_->vmaAllocator(), ring_buffers_[slot], ring_allocs_[slot]);
                ring_buffers_[slot] = VK_NULL_HANDLE;
                ring_allocs_[slot] = nullptr;
                ring_mapped_[slot] = nullptr;
                ring_sizes_[slot] = 0;
            }

            const VkDeviceSize alloc_size = required * 2u;
            VkBuffer buf{VK_NULL_HANDLE};
            VmaAllocation alloc{nullptr};
            void* mapped{nullptr};
            if (!createGpuBufferVmaBuffer(
                    device_ctx_->vmaAllocator(),
                    alloc_size,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    /*cpu_writable=*/true,
                    &buf,
                    &alloc,
                    &mapped) ||
                buf == VK_NULL_HANDLE || mapped == nullptr)
            {
                // Leave the slot null rather than publishing a half-built handle.
                if (buf != VK_NULL_HANDLE)
                    destroyGpuBufferVmaBuffer(device_ctx_->vmaAllocator(), buf, alloc);
                return;
            }
            // Fill all-active so the tail past mask_ (unread by cull) never reads 0.
            std::memset(mapped, 0xFF, static_cast<std::size_t>(alloc_size));
            ring_buffers_[slot] = buf;
            ring_allocs_[slot] = alloc;
            ring_mapped_[slot] = mapped;
            ring_sizes_[slot] = alloc_size;
        }

        // Persistent host-coherent mapping; the implicit host-write barrier at queue
        // submit makes the memcpy visible to this frame's cull dispatch.
        if (ring_mapped_[slot] != nullptr && !mask_.empty())
            std::memcpy(ring_mapped_[slot], mask_.data(), static_cast<std::size_t>(mask_.size()) * sizeof(uint32_t));
    }

    VkBuffer SpatialCullGrid::activeMaskBuffer() const noexcept
    {
        if (ring_buffers_.empty() || current_slot_ >= ring_buffers_.size())
            return VK_NULL_HANDLE;
        return ring_buffers_[current_slot_];
    }

    uint64_t SpatialCullGrid::activeMaskAddress() const noexcept
    {
        if (device_ctx_ == nullptr || ring_buffers_.empty() || current_slot_ >= ring_buffers_.size())
            return 0ull;
        const VkBuffer buf = ring_buffers_[current_slot_];
        if (buf == VK_NULL_HANDLE)
            return 0ull;
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = buf;
        // Buffers are created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT and the
        // allocator with VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT (VulkanContext),
        // so the address is valid. Core in Vulkan 1.2 (the device targets 1.3).
        return vkGetBufferDeviceAddress(device_ctx_->logicalDevice(), &info);
    }

    const uint32_t* SpatialCullGrid::gpuMaskMappedForTest() const noexcept
    {
        if (ring_mapped_.empty() || current_slot_ >= ring_mapped_.size())
            return nullptr;
        return static_cast<const uint32_t*>(ring_mapped_[current_slot_]);
    }

} // namespace lux::render
