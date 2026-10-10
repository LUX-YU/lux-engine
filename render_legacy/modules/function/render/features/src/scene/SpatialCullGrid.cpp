#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp> // DeviceContext::vmaAllocator()
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/scene/SpatialCullGrid.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lux::render
{
    namespace
    {
        struct MaskCandidate
        {
            VmaBuffer buffer;
            void* mapped;
            VkDeviceSize size;
            VkDeviceAddress address;
        };

        Expected<MaskCandidate> prepareMask(DeviceContext& device, VkDeviceSize size) noexcept
        {
            VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer_info.size = size;
            buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            VmaAllocationCreateInfo allocation_info{};
            allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
            allocation_info.flags =
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            // The persistent ring is written only in a GPU-idle slot. Coherence makes
            // publication after memcpy infallible; no ignored flush error or stale mask.
            allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            VmaBuffer::Allocation allocation{device.vmaAllocator()};
            VmaAllocationInfo mapping{};
            const auto allocated = vmaCreateBuffer(
                allocation.allocator,
                &buffer_info,
                &allocation_info,
                &allocation.buffer,
                &allocation.allocation,
                &mapping
            );
            if (allocated != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(allocated));
            }
            auto buffer = VmaBuffer::adopt(allocation);
            if (!mapping.pMappedData)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_MEMORY_MAP_FAILED));
            }
            VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            address_info.buffer = buffer.buffer();
            const auto address = vkGetBufferDeviceAddress(device.logicalDevice(), &address_info);
            if (address == 0)
            {
                return renderFailure<err::memory::InvalidBufferConfiguration>();
            }
            std::memset(mapping.pMappedData, 0xFF, static_cast<std::size_t>(size));
            return MaskCandidate{std::move(buffer), mapping.pMappedData, size, address};
        }
    } // namespace

    SpatialCullGrid::CreateResult SpatialCullGrid::create(const CreateInfo& info) noexcept
    {
        const bool is_invalid_frames = info.frames_in_flight == 0 || info.frames_in_flight > kMaxFramesInFlight;
        const bool is_invalid_capacity = info.initial_capacity == 0;
        const bool is_invalid_cell = !std::isfinite(info.cell_size) || info.cell_size <= 0.0f;
        const bool is_invalid_distance = !std::isfinite(info.cull_distance) || info.cull_distance <= 0.0f;
        const bool is_invalid_configuration =
            is_invalid_frames || is_invalid_capacity || is_invalid_cell || is_invalid_distance;
        if (is_invalid_configuration)
        {
            return renderFailure<err::memory::InvalidBufferConfiguration>();
        }
        const auto count = info.frames_in_flight + 1u;
        const auto size = VkDeviceSize(info.initial_capacity) * sizeof(uint32_t);
        std::vector<MaskCandidate> candidates;
        candidates.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            auto candidate = prepareMask(info.device, size);
            if (!candidate)
            {
                return lux::cxx::unexpected(candidate.error());
            }
            candidates.push_back(std::move(*candidate));
        }
        std::vector<RingSlot> ring;
        ring.reserve(count);
        for (auto& candidate : candidates)
        {
            const auto allocation = candidate.buffer.release();
            ring.push_back(RingSlot{
                TFifOwnedAllocated<VkBuffer>{info.retirement, allocation.buffer, allocation.allocation},
                candidate.mapped,
                candidate.size,
                candidate.address
            });
        }
        return std::unique_ptr<SpatialCullGrid>(new SpatialCullGrid(info, std::move(ring)));
    }

    SpatialCullGrid::SpatialCullGrid(const CreateInfo& info, std::vector<RingSlot> ring) noexcept
        : device_(info.device), retirement_(info.retirement), cell_size_(info.cell_size),
          cull_distance_(info.cull_distance), ring_(std::move(ring))
    {
    }

    void SpatialCullGrid::setCellSize(float s) noexcept
    {
        if (std::isfinite(s) && s > 0.0f)
        {
            cell_size_ = s;
        }
    }

    void SpatialCullGrid::setCullDistance(float r) noexcept
    {
        if (std::isfinite(r) && r > 0.0f)
        {
            cull_distance_ = r;
        }
    }

    // =========================================================================
    //  Pure cell math (no GPU / no state — CPU-unit-testable)
    // =========================================================================
    void SpatialCullGrid::cellCoord(
        float world_x,
        float world_y,
        float cell_size,
        int32_t& out_cx,
        int32_t& out_cy
    ) noexcept
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
        {
            return true; // no cull source → keep active (never over-cull)
        }
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
            {
                return true;
            }
        }
        return false;
    }

    // =========================================================================
    //  Per-frame update
    // =========================================================================
    Expected<void> SpatialCullGrid::update(
        uint64_t serial,
        std::span<const std::array<float, 3>> cameras,
        const InstanceResources& instances
    ) noexcept
    {
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
            {
                continue;
            }
            const auto& cull = instances.cullMetaAt(InstanceSlot{slot});
            if (cull.bsphere[3] < 0.0f)
            {
                continue;
            }
            // SpatialCullGrid is a temporary coarse-mask compatibility path;
            // reconstruct only at its 128m-cell boundary. Fine view/HZB/shadow
            // culling retains exact page/local coordinates on the GPU.
            extract_scratch_.push_back(InstanceXY{
                slot,
                static_cast<float>(cull.bsphere_page[0]) * page_size + cull.bsphere[0],
                static_cast<float>(cull.bsphere_page[2]) * page_size + cull.bsphere[2]
            });
        }
        return update(serial, cameras, extract_scratch_, slot_count);
    }

    Expected<void> SpatialCullGrid::update(
        uint64_t serial,
        std::span<const std::array<float, 3>> cameras,
        std::span<const InstanceXY> alive,
        uint32_t slot_count
    ) noexcept
    {
        if (serial == last_upload_serial_)
        {
            return {};
        }
        const auto next_slot = (current_slot_ + 1u) % static_cast<uint32_t>(ring_.size());
        const auto required = VkDeviceSize(std::max(slot_count, 1u)) * sizeof(uint32_t);
        auto& slot = ring_[next_slot];
        if (required > slot.size)
        {
            auto candidate = prepareMask(device_, required * 2u);
            if (!candidate)
            {
                return lux::cxx::unexpected(candidate.error());
            }
            const auto allocation = candidate->buffer.release();
            slot = RingSlot{
                TFifOwnedAllocated<VkBuffer>{retirement_, allocation.buffer, allocation.allocation},
                candidate->mapped,
                candidate->size,
                candidate->address
            };
        }
        // All recoverable work succeeded. The reused slot is FIF+1 frames old;
        // its coherent mapping and the complete CPU mask are published together.
        recomputeMask(cameras, alive, slot_count);
        if (!mask_.empty())
        {
            std::memcpy(slot.mapped, mask_.data(), mask_.size() * sizeof(uint32_t));
        }
        current_slot_ = next_slot;
        last_upload_serial_ = serial;
        return {};
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
        auto cellActive = [&](CellKey key) -> bool
        {
            const auto it = cell_active_.find(key);
            if (it != cell_active_.end())
            {
                return it->second != 0u;
            }
            const bool active = cellInRange(key.x, key.y, cell_size_, cull_distance_, cameras);
            cell_active_.emplace(key, active ? 1u : 0u);
            return active;
        };

        for (const auto& inst : alive)
        {
            if (inst.slot >= slot_count)
            {
                continue;
            }
            CellKey key{};
            cellCoord(inst.x, inst.y, cell_size_, key.x, key.y);
            if (cellActive(key))
            {
                ++stat_active_instances_;
            }
            else
            {
                mask_[inst.slot] = 0u; // dormant cell → masked out of every GPU cull dispatch
            }
        }

        stat_total_cells_ = static_cast<uint32_t>(cell_active_.size());
        for (const auto& [k, v] : cell_active_)
        {
            (void)k;
            if (v)
            {
                ++stat_active_cells_;
            }
        }
    }

    VkBuffer SpatialCullGrid::activeMaskBuffer() const noexcept
    {
        return ring_[current_slot_].buffer.get();
    }

    uint64_t SpatialCullGrid::activeMaskAddress() const noexcept
    {
        return ring_[current_slot_].address;
    }

    const uint32_t* SpatialCullGrid::gpuMaskMappedForTest() const noexcept
    {
        return static_cast<const uint32_t*>(ring_[current_slot_].mapped);
    }
} // namespace lux::render
