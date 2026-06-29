#include <lux/engine/render/resources/point_cloud/PointCloudGlobalBuffer.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

namespace lux::render
{

// ============================================================================
//  init / shutdown
// ============================================================================

bool PointCloudGlobalBuffer::init(VmaAllocator allocator, uint32_t max_points)
{
    if (isInitialized()) return true;
    if (!allocator || max_points == 0) return false;

    allocator_  = allocator;
    max_points_ = max_points;

    const VkDeviceSize buf_size =
        static_cast<VkDeviceSize>(max_points) * sizeof(GpuPointVertex);

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size        = buf_size;
    bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT   // Simple draw path
                    | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT  // SSBO path (GPU-driven / LOD)
                    | VK_BUFFER_USAGE_TRANSFER_DST_BIT    // Staging target
                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;   // Buffer growth copy source
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    const VkResult res = vmaCreateBuffer(allocator_, &bci, &aci,
                                         &buffer_, &allocation_, nullptr);
    if (res != VK_SUCCESS)
    {
        buffer_     = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
        return false;
    }

    // Entire buffer is one big free region initially
    free_list_.push_back({ 0, max_points_ });
    return true;
}

void PointCloudGlobalBuffer::shutdown()
{
    if (!isInitialized()) return;

    vmaDestroyBuffer(allocator_, buffer_, allocation_);
    buffer_     = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    allocator_  = nullptr;
    max_points_ = 0;

    slots_.clear();
    free_list_.clear();
}

// ============================================================================
//  Slot management
// ============================================================================

uint32_t PointCloudGlobalBuffer::tryAllocFromFreeList(uint32_t capacity)
{
    // First-fit: walk free_list_ looking for a region large enough
    for (auto it = free_list_.begin(); it != free_list_.end(); ++it)
    {
        if (it->capacity >= capacity)
        {
            uint32_t first = it->first;
            if (it->capacity == capacity)
            {
                free_list_.erase(it);
            }
            else
            {
                it->first    += capacity;
                it->capacity -= capacity;
            }
            return first;
        }
    }
    return kInvalidChunkId; // reused as sentinel for "not found"
}

void PointCloudGlobalBuffer::returnToFreeList(uint32_t first, uint32_t capacity)
{
    // Insert sorted by first offset so adjacent regions can be merged
    auto pos = std::lower_bound(free_list_.begin(), free_list_.end(), first,
        [](const FreeRegion& r, uint32_t v) { return r.first < v; });
    auto it = free_list_.insert(pos, { first, capacity });

    // Merge with next region
    auto next = std::next(it);
    if (next != free_list_.end() && it->first + it->capacity == next->first)
    {
        it->capacity += next->capacity;
        free_list_.erase(next);
    }
    // Merge with previous region
    if (it != free_list_.begin())
    {
        auto prev = std::prev(it);
        if (prev->first + prev->capacity == it->first)
        {
            prev->capacity += it->capacity;
            free_list_.erase(it);
        }
    }
}

bool PointCloudGlobalBuffer::reserveSlot(uint32_t chunk_id, uint32_t capacity)
{
    assert(isInitialized());
    if (capacity == 0) return false;
    if (slots_.contains(chunk_id)) return true; // already reserved

    const uint32_t first = tryAllocFromFreeList(capacity);
    if (first == kInvalidChunkId) return false; // buffer full

    slots_.insert(chunk_id, Slot{ first, capacity, 0 });
    return true;
}

void PointCloudGlobalBuffer::freeSlot(uint32_t chunk_id)
{
    if (!slots_.contains(chunk_id)) return;
    const auto slot = slots_.at(chunk_id);
    (void)slots_.erase(chunk_id);
    retire_scheduler_->defer(deferred_queue_->currentSerial(), retire_owner_token_,
        [this, first = slot.first_point, cap = slot.capacity]{ returnToFreeList(first, cap); });
}

void PointCloudGlobalBuffer::freeAllSlots()
{
    // Mirror freeSlot(): defer each region's return-to-free-list by the current
    // GPU serial rather than reclaiming it synchronously. The backing buffer is
    // GPU_ONLY and persistent, so a prior in-flight frame's draw may still be
    // reading these offsets. An immediate returnToFreeList would let a same-sweep
    // clearAll-then-upload (both handled in one submitTransfers) re-allocate the
    // region and vkCmdCopyBuffer over data the GPU is still consuming.
    assert(retire_scheduler_ && deferred_queue_);
    const auto& vals = slots_.values();
    const std::size_t count = vals.size();
    const auto serial = deferred_queue_->currentSerial();
    for (std::size_t i = 0; i < count; ++i)
        retire_scheduler_->defer(serial, retire_owner_token_,
            [this, first = vals[i].first_point, cap = vals[i].capacity]{ returnToFreeList(first, cap); });
    slots_.clear();
}

void PointCloudGlobalBuffer::resetSlot(uint32_t chunk_id) noexcept
{
    if (!slots_.contains(chunk_id)) return;
    slots_.at(chunk_id).point_count = 0;
}

void PointCloudGlobalBuffer::setPointCount(uint32_t chunk_id, uint32_t point_count) noexcept
{
    if (!slots_.contains(chunk_id)) return;
    slots_.at(chunk_id).point_count = point_count;
}

std::optional<PointCloudGlobalBuffer::Slot>
PointCloudGlobalBuffer::getSlot(uint32_t chunk_id) const noexcept
{
    if (!slots_.contains(chunk_id)) return std::nullopt;
    return slots_.at(chunk_id);
}

uint32_t PointCloudGlobalBuffer::usedPoints() const noexcept
{
    uint32_t free = 0;
    for (const auto& r : free_list_) free += r.capacity;
    return max_points_ - free;
}

// ============================================================================
//  TransferScheduler-aware overloads
// ============================================================================

bool PointCloudGlobalBuffer::ensureSlotCapacity(uint32_t chunk_id,
                                                uint32_t capacity,
                                                TransferScheduler& scheduler)
{
    assert(isInitialized());
    if (capacity == 0) return false;

    if (!slots_.contains(chunk_id))
    {
        const uint32_t first = allocOrGrow(capacity, scheduler);
        if (first == kInvalidChunkId) return false;
        slots_.insert(chunk_id, Slot{ first, capacity, 0 });
        return true;
    }

    Slot& slot = slots_.at(chunk_id);
    if (slot.capacity >= capacity)
        return true;

    const uint32_t first = allocOrGrow(capacity, scheduler);
    if (first == kInvalidChunkId) return false;

    const uint32_t old_first = slot.first_point;
    const uint32_t old_cap   = slot.capacity;
    retire_scheduler_->defer(deferred_queue_->currentSerial(), retire_owner_token_,
        [this, old_first, old_cap]{ returnToFreeList(old_first, old_cap); });
    slot.first_point = first;
    slot.capacity = capacity;
    slot.point_count = 0;
    return true;
}

bool PointCloudGlobalBuffer::upload(uint32_t chunk_id,
                                    std::span<const GpuPointVertex> data,
                                    TransferScheduler& scheduler)
{
    if (data.empty()) return false;
    if (!slots_.contains(chunk_id)) return false;

    Slot& slot = slots_.at(chunk_id);
    const uint32_t count = static_cast<uint32_t>(data.size());
    assert(count <= slot.capacity && "Upload exceeds allocated slot capacity");
    // Release guard: if capacity growth failed earlier (VMA OOM or the uint32
    // ceiling in allocOrGrow) the assert above is compiled out, and the copy below
    // would spill count*stride bytes past this slot into the neighbouring chunk —
    // or past the buffer entirely for a tail slot (validation error / device loss).
    // Drop the upload instead. (C-2)
    if (count > slot.capacity)
        return false;

    const VkDeviceSize byte_offset =
        static_cast<VkDeviceSize>(slot.first_point) * sizeof(GpuPointVertex);
    const VkDeviceSize byte_size =
        static_cast<VkDeviceSize>(count) * sizeof(GpuPointVertex);

    StagingAlloc stg = scheduler.allocateStaging(byte_size);
    if (!stg.mapped) return false;

    std::memcpy(stg.mapped, data.data(), byte_size);

    scheduler.submitBufferCopy({
        .src        = stg.buffer,
        .src_offset = stg.srcOffset,
        .dst        = buffer_,
        .dst_offset = byte_offset,
        .size       = byte_size,
        .domain     = EBufferDomain::VertexInput_CS,
        .priority   = 0,
    });

    slot.point_count = count;
    return true;
}

uint32_t PointCloudGlobalBuffer::allocOrGrow(uint32_t capacity,
                                             TransferScheduler& scheduler)
{
    uint32_t first = tryAllocFromFreeList(capacity);
    if (first != kInvalidChunkId)
        return first;

    if (!growBuffer(capacity, scheduler))
        return kInvalidChunkId;

    return tryAllocFromFreeList(capacity);
}

bool PointCloudGlobalBuffer::growBuffer(uint32_t required_capacity,
                                        TransferScheduler& scheduler)
{
    if (!isInitialized() || required_capacity == 0)
        return false;

    uint64_t new_capacity = static_cast<uint64_t>(max_points_);
    const uint64_t min_required = static_cast<uint64_t>(required_capacity);

    while (new_capacity < min_required)
    {
        const uint64_t doubled = new_capacity * 2ull;
        if (doubled <= new_capacity)
        {
            new_capacity = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
            break;
        }
        new_capacity = doubled;
    }

    if (new_capacity == static_cast<uint64_t>(max_points_))
        new_capacity = std::min<uint64_t>(
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()),
            std::max<uint64_t>(static_cast<uint64_t>(max_points_) * 2ull,
                               static_cast<uint64_t>(max_points_) + min_required));

    if (new_capacity > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
        new_capacity = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());

    if (new_capacity <= static_cast<uint64_t>(max_points_))
        return false;

    VkBuffer new_buffer = VK_NULL_HANDLE;
    VmaAllocation new_allocation = VK_NULL_HANDLE;

    const VkDeviceSize new_size =
        static_cast<VkDeviceSize>(new_capacity) * sizeof(GpuPointVertex);

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size        = new_size;
    bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                    | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                    | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    const VkResult create_res = vmaCreateBuffer(
        allocator_, &bci, &aci, &new_buffer, &new_allocation, nullptr);
    if (create_res != VK_SUCCESS)
        return false;

    // Submit old→new growth copy at priority=-1. The scheduler's mid-barrier
    // (TRANSFER_WRITE → TRANSFER_WRITE) ensures this completes before
    // subsequent data copies at priority=0.
    const VkDeviceSize old_size =
        static_cast<VkDeviceSize>(max_points_) * sizeof(GpuPointVertex);
    scheduler.submitBufferCopy({
        .src        = buffer_,
        .src_offset = 0,
        .dst        = new_buffer,
        .dst_offset = 0,
        .size       = old_size,
        .domain     = EBufferDomain::TransferDst,
        .priority   = -1,
    });

    deferred_queue_->retireBuffer(buffer_, allocation_);

    buffer_ = new_buffer;
    allocation_ = new_allocation;
    returnToFreeList(max_points_, static_cast<uint32_t>(new_capacity) - max_points_);
    max_points_ = static_cast<uint32_t>(new_capacity);
    return true;
}

} // namespace lux::render
