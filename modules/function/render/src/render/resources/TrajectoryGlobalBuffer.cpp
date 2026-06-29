#include <lux/engine/render/resources/TrajectoryGlobalBuffer.hpp>
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

bool TrajectoryGlobalBuffer::init(VmaAllocator allocator, uint32_t max_vertices)
{
    if (isInitialized()) return true;
    if (!allocator || max_vertices == 0) return false;

    allocator_    = allocator;
    max_vertices_ = max_vertices;

    const VkDeviceSize buf_size =
        static_cast<VkDeviceSize>(max_vertices) * sizeof(GpuTrajectoryVertex);

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size        = buf_size;
    bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                    | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                    | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
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

    free_list_.push_back({ 0, max_vertices_ });
    return true;
}

void TrajectoryGlobalBuffer::shutdown()
{
    if (!isInitialized()) return;

    vmaDestroyBuffer(allocator_, buffer_, allocation_);
    buffer_     = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    allocator_  = nullptr;
    max_vertices_ = 0;

    slots_.clear();
    free_list_.clear();
}

// ============================================================================
//  Freelist management
// ============================================================================

uint32_t TrajectoryGlobalBuffer::tryAllocFromFreeList(uint32_t capacity)
{
    for (auto it = free_list_.begin(); it != free_list_.end(); ++it)
    {
        if (it->capacity >= capacity)
        {
            uint32_t first = it->first;
            if (it->capacity == capacity)
                free_list_.erase(it);
            else
            {
                it->first    += capacity;
                it->capacity -= capacity;
            }
            return first;
        }
    }
    return kInvalidTrajectoryId;
}

void TrajectoryGlobalBuffer::returnToFreeList(uint32_t first, uint32_t capacity)
{
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

// ============================================================================
//  Slot management
// ============================================================================

bool TrajectoryGlobalBuffer::reserveSlot(uint32_t trajectory_id, uint32_t capacity)
{
    assert(isInitialized());
    if (capacity == 0) return false;
    if (slots_.contains(trajectory_id)) return true;

    const uint32_t first = tryAllocFromFreeList(capacity);
    if (first == kInvalidTrajectoryId) return false;

    slots_.insert(trajectory_id, Slot{ first, capacity, 0 });
    return true;
}

void TrajectoryGlobalBuffer::freeSlot(uint32_t trajectory_id)
{
    if (!slots_.contains(trajectory_id)) return;
    const auto slot = slots_.at(trajectory_id);
    (void)slots_.erase(trajectory_id);
    retire_scheduler_->defer(deferred_queue_->currentSerial(), retire_owner_token_,
        [this, first = slot.first_vertex, cap = slot.capacity]{ returnToFreeList(first, cap); });
}

void TrajectoryGlobalBuffer::clearTrajectory(uint32_t trajectory_id) noexcept
{
    if (!slots_.contains(trajectory_id)) return;
    slots_.at(trajectory_id).vertex_count = 0;
}

void TrajectoryGlobalBuffer::setVertexCount(uint32_t trajectory_id, uint32_t count) noexcept
{
    if (!slots_.contains(trajectory_id)) return;
    auto& slot = slots_.at(trajectory_id);
    slot.vertex_count = std::min(count, slot.capacity);
}

// ============================================================================
//  Queries
// ============================================================================

std::optional<TrajectoryGlobalBuffer::Slot>
TrajectoryGlobalBuffer::getSlot(uint32_t trajectory_id) const noexcept
{
    if (!slots_.contains(trajectory_id)) return std::nullopt;
    return slots_.at(trajectory_id);
}

uint32_t TrajectoryGlobalBuffer::usedVertices() const noexcept
{
    uint32_t free = 0;
    for (const auto& r : free_list_) free += r.capacity;
    return max_vertices_ - free;
}

// ============================================================================
//  TransferScheduler-aware overloads
// ============================================================================

bool TrajectoryGlobalBuffer::ensureSlotCapacity(uint32_t trajectory_id,
                                                uint32_t capacity,
                                                TransferScheduler& scheduler)
{
    assert(isInitialized());
    if (capacity == 0) return false;

    if (!slots_.contains(trajectory_id))
    {
        const uint32_t init_cap = std::max(capacity, 64u);
        const uint32_t first = allocOrGrow(init_cap, scheduler);
        if (first == kInvalidTrajectoryId) return false;
        slots_.insert(trajectory_id, Slot{ first, init_cap, 0 });
        return true;
    }

    Slot& slot = slots_.at(trajectory_id);
    if (slot.capacity >= capacity)
        return true;

    const uint32_t grow_cap = std::max(capacity, slot.capacity * 2u);
    // Capture the buffer BEFORE allocOrGrow: if it grows, growBuffer() replaces
    // buffer_ with a new VkBuffer and submits the whole-buffer old->new copy at
    // priority -1. Reading the slot-relocation copy below from the NEW buffer
    // would race that growth copy (both priority -1, no barrier between same-
    // priority copies -> RAW: the relocation could read bytes the growth copy
    // hasn't written yet). The old buffer is only deferred-retired, so it is
    // still alive this frame; relocate FROM it instead — no dependency on the
    // growth copy. When no grow happens, pre_grow_buffer == buffer_ (a normal
    // same-buffer, non-overlapping relocation). (#26)
    const VkBuffer pre_grow_buffer = buffer_;
    const uint32_t first = allocOrGrow(grow_cap, scheduler);
    if (first == kInvalidTrajectoryId) return false;

    // Copy existing vertex data from old region to new region.
    const uint32_t old_first = slot.first_vertex;
    const uint32_t old_count = slot.vertex_count;
    if (old_count > 0)
    {
        scheduler.submitBufferCopy({
            .src        = pre_grow_buffer,
            .src_offset = static_cast<VkDeviceSize>(old_first) * sizeof(GpuTrajectoryVertex),
            .dst        = buffer_,
            .dst_offset = static_cast<VkDeviceSize>(first) * sizeof(GpuTrajectoryVertex),
            .size       = static_cast<VkDeviceSize>(old_count) * sizeof(GpuTrajectoryVertex),
            .domain     = EBufferDomain::TransferDst,
            .priority   = -1,
        });
    }

    retire_scheduler_->defer(deferred_queue_->currentSerial(), retire_owner_token_,
        [this, old_first, old_cap = slot.capacity]{ returnToFreeList(old_first, old_cap); });
    slot.first_vertex = first;
    slot.capacity     = grow_cap;
    slot.vertex_count = old_count;
    return true;
}

bool TrajectoryGlobalBuffer::upload(uint32_t trajectory_id,
                                    std::span<const GpuTrajectoryVertex> data,
                                    TransferScheduler& scheduler)
{
    if (data.empty()) return false;
    if (!slots_.contains(trajectory_id)) return false;

    Slot& slot = slots_.at(trajectory_id);
    const uint32_t count = static_cast<uint32_t>(data.size());
    assert(count <= slot.capacity && "Upload exceeds allocated slot capacity");
    // Release guard: if capacity growth failed earlier (VMA OOM or the uint32
    // ceiling) the assert is compiled out and the copy below would spill past this
    // slot into the neighbouring trajectory. Drop the upload instead. (C-2)
    if (count > slot.capacity)
        return false;

    const VkDeviceSize byte_offset =
        static_cast<VkDeviceSize>(slot.first_vertex) * sizeof(GpuTrajectoryVertex);
    const VkDeviceSize byte_size =
        static_cast<VkDeviceSize>(count) * sizeof(GpuTrajectoryVertex);

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

    slot.vertex_count = count;
    return true;
}

uint32_t TrajectoryGlobalBuffer::append(uint32_t trajectory_id,
                                        std::span<const GpuTrajectoryVertex> data,
                                        TransferScheduler& scheduler)
{
    if (data.empty()) return 0;
    if (!slots_.contains(trajectory_id)) return 0;

    Slot& slot = slots_.at(trajectory_id);
    const uint32_t remaining = slot.capacity - slot.vertex_count;
    if (remaining == 0) return 0;

    const uint32_t to_append = std::min(static_cast<uint32_t>(data.size()), remaining);
    const VkDeviceSize byte_offset =
        static_cast<VkDeviceSize>(slot.first_vertex + slot.vertex_count) * sizeof(GpuTrajectoryVertex);
    const VkDeviceSize byte_size =
        static_cast<VkDeviceSize>(to_append) * sizeof(GpuTrajectoryVertex);

    StagingAlloc stg = scheduler.allocateStaging(byte_size);
    if (!stg.mapped) return 0;

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

    slot.vertex_count += to_append;
    return to_append;
}

uint32_t TrajectoryGlobalBuffer::allocOrGrow(uint32_t capacity,
                                             TransferScheduler& scheduler)
{
    uint32_t first = tryAllocFromFreeList(capacity);
    if (first != kInvalidTrajectoryId)
        return first;

    if (!growBuffer(capacity, scheduler))
        return kInvalidTrajectoryId;

    return tryAllocFromFreeList(capacity);
}

bool TrajectoryGlobalBuffer::growBuffer(uint32_t required_capacity,
                                        TransferScheduler& scheduler)
{
    if (!isInitialized() || required_capacity == 0)
        return false;

    uint64_t new_capacity = static_cast<uint64_t>(max_vertices_);
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

    if (new_capacity == static_cast<uint64_t>(max_vertices_))
        new_capacity = std::min<uint64_t>(
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()),
            std::max<uint64_t>(static_cast<uint64_t>(max_vertices_) * 2ull,
                               static_cast<uint64_t>(max_vertices_) + min_required));

    if (new_capacity > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
        new_capacity = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());

    if (new_capacity <= static_cast<uint64_t>(max_vertices_))
        return false;

    VkBuffer new_buffer = VK_NULL_HANDLE;
    VmaAllocation new_allocation = VK_NULL_HANDLE;

    const VkDeviceSize new_size =
        static_cast<VkDeviceSize>(new_capacity) * sizeof(GpuTrajectoryVertex);

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

    const VkDeviceSize old_size =
        static_cast<VkDeviceSize>(max_vertices_) * sizeof(GpuTrajectoryVertex);
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
    returnToFreeList(max_vertices_, static_cast<uint32_t>(new_capacity) - max_vertices_);
    max_vertices_ = static_cast<uint32_t>(new_capacity);
    return true;
}

} // namespace lux::render
