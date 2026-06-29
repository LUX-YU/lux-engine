#include <lux/engine/render/resources/memory/ArenaAllocator.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cassert>
#include <utility>

namespace lux::render
{

ArenaAllocator::ArenaAllocator(uint64_t total_capacity)
    : total_capacity_(total_capacity)
{
    if (total_capacity > 0)
    {
        VmaVirtualBlockCreateInfo ci{};
        ci.size = total_capacity;
        if (vmaCreateVirtualBlock(&ci, &block_) != VK_SUCCESS)
            block_ = nullptr;
    }
}

ArenaAllocator::ArenaAllocator() = default;

ArenaAllocator::~ArenaAllocator()
{
    if (block_)
    {
        vmaClearVirtualBlock(block_);
        vmaDestroyVirtualBlock(block_);
        block_ = nullptr;
    }
}

ArenaAllocator::ArenaAllocator(ArenaAllocator&& o) noexcept
    : block_(o.block_)
    , total_capacity_(o.total_capacity_)
    , live_allocs_(std::move(o.live_allocs_))
{
    o.block_ = nullptr;
    o.total_capacity_ = 0;
}

ArenaAllocator& ArenaAllocator::operator=(ArenaAllocator&& o) noexcept
{
    if (this != &o)
    {
        if (block_)
        {
            vmaClearVirtualBlock(block_);
            vmaDestroyVirtualBlock(block_);
        }
        block_ = o.block_;
        total_capacity_ = o.total_capacity_;
        live_allocs_ = std::move(o.live_allocs_);
        o.block_ = nullptr;
        o.total_capacity_ = 0;
    }
    return *this;
}

ArenaAllocator::Allocation ArenaAllocator::allocate(uint64_t size, uint64_t alignment)
{
    if (size == 0 || !block_)
        return {};

    // VMA virtual blocks require power-of-two alignment.
    // When the caller requests a non-power-of-2 alignment (e.g. lcm(256, stride)),
    // over-allocate so we can manually align the returned offset.
    const bool is_pow2 = alignment > 0 && (alignment & (alignment - 1)) == 0;
    const uint64_t vma_align = is_pow2 ? alignment : nextPow2(alignment);
    const uint64_t extra     = is_pow2 ? 0 : (alignment - 1); // worst-case padding
    const uint64_t alloc_size = size + extra;

    VmaVirtualAllocationCreateInfo ci{};
    ci.size      = alloc_size;
    ci.alignment = vma_align;

    VmaVirtualAllocation handle{};
    VkDeviceSize raw_offset = 0;
    if (vmaVirtualAllocate(block_, &ci, &handle, &raw_offset) != VK_SUCCESS)
        return {};

    // Adjust offset so it satisfies the original (possibly non-pow2) alignment.
    uint64_t offset = static_cast<uint64_t>(raw_offset);
    if (!is_pow2 && (offset % alignment) != 0)
        offset = ((offset + alignment - 1) / alignment) * alignment;

    // Keep live list sorted by offset for planDefragmentation().
    LiveEntry entry{ offset, size, handle };
    auto it = std::lower_bound(
        live_allocs_.begin(),
        live_allocs_.end(),
        entry.offset,
        [](const LiveEntry& e, uint64_t off) { return e.offset < off; });
    live_allocs_.insert(it, entry);

    return { offset, size, handle };
}

void ArenaAllocator::free(const Allocation& alloc)
{
    if (!alloc.valid() || !block_)
        return;

    vmaVirtualFree(block_, alloc.handle);

    // Remove from live list
    auto it = std::lower_bound(
        live_allocs_.begin(),
        live_allocs_.end(),
        alloc.offset,
        [](const LiveEntry& e, uint64_t off) { return e.offset < off; });
    if (it != live_allocs_.end() && it->offset == alloc.offset)
        live_allocs_.erase(it);
}

float ArenaAllocator::fragmentationRatio() const
{
    if (!block_)
        return 0.0f;

    VmaDetailedStatistics stats{};
    vmaCalculateVirtualBlockStatistics(block_, &stats);

    const uint64_t total_free = total_capacity_ - stats.statistics.allocationBytes;
    if (total_free == 0)
        return 0.0f;
    if (stats.statistics.blockCount <= 1 && stats.statistics.allocationCount == 0)
        return 0.0f;

    // Largest free region vs total free.
    uint64_t largest_free = 0;
    uint64_t cursor = 0;
    for (const auto& la : live_allocs_)
    {
        if (la.offset > cursor)
            largest_free = std::max(largest_free, la.offset - cursor);
        cursor = la.offset + la.size;
    }
    if (total_capacity_ > cursor)
        largest_free = std::max(largest_free, total_capacity_ - cursor);

    if (largest_free >= total_free)
        return 0.0f;
    return 1.0f - static_cast<float>(largest_free) / static_cast<float>(total_free);
}

std::vector<ArenaAllocator::CopyOp> ArenaAllocator::planDefragmentation() const
{
    std::vector<CopyOp> ops;
    uint64_t dst_cursor = 0;

    for (const auto& la : live_allocs_)
    {
        if (la.offset != dst_cursor)
            ops.push_back(CopyOp{ la.offset, dst_cursor, la.size });
        dst_cursor += la.size;
    }

    return ops;
}

void ArenaAllocator::applyDefragmentation(const std::vector<CopyOp>& /*ops*/)
{
    if (!block_)
        return;

    // Collect sizes of live allocations in order
    std::vector<uint64_t> sizes;
    sizes.reserve(live_allocs_.size());
    for (const auto& la : live_allocs_)
        sizes.push_back(la.size);

    // Rebuild: clear the block and re-allocate sequentially from 0
    vmaClearVirtualBlock(block_);
    live_allocs_.clear();

    uint64_t cursor = 0;
    for (uint64_t sz : sizes)
    {
        VmaVirtualAllocationCreateInfo ci{};
        ci.size      = sz;
        ci.alignment = 1;

        VmaVirtualAllocation handle{};
        VkDeviceSize offset = 0;
        VkResult r = vmaVirtualAllocate(block_, &ci, &handle, &offset);
        assert(r == VK_SUCCESS && "re-allocation during defrag must succeed");
        (void)r;

        // After compaction, allocations should be sequential
        assert(static_cast<uint64_t>(offset) == cursor);
        live_allocs_.push_back({ cursor, sz, handle });
        cursor += sz;
    }
}

uint64_t ArenaAllocator::totalCapacity() const
{
    return total_capacity_;
}

uint64_t ArenaAllocator::usedBytes() const
{
    if (!block_)
        return 0;

    VmaStatistics stats{};
    vmaGetVirtualBlockStatistics(block_, &stats);
    return stats.allocationBytes;
}

uint64_t ArenaAllocator::freeBytes() const
{
    return total_capacity_ - usedBytes();
}

size_t ArenaAllocator::freeBlockCount() const
{
    // Not directly available from VMA; approximate from live allocs.
    // Number of gaps = number of live allocs + 1 (edges) minus contiguous runs.
    if (live_allocs_.empty())
        return total_capacity_ > 0 ? 1 : 0;

    size_t gaps = 0;
    uint64_t cursor = 0;
    for (const auto& la : live_allocs_)
    {
        if (la.offset > cursor)
            ++gaps;
        cursor = la.offset + la.size;
    }
    if (cursor < total_capacity_)
        ++gaps;
    return gaps;
}

bool ArenaAllocator::isFree(uint64_t offset, uint64_t size) const
{
    // A region is free if no live allocation overlaps it.
    for (const auto& la : live_allocs_)
    {
        if (la.offset + la.size <= offset)
            continue;
        if (la.offset >= offset + size)
            break; // sorted
        return false; // overlap
    }
    return offset + size <= total_capacity_;
}

VmaVirtualAllocation ArenaAllocator::findHandleAt(uint64_t offset) const
{
    auto it = std::lower_bound(
        live_allocs_.begin(),
        live_allocs_.end(),
        offset,
        [](const LiveEntry& e, uint64_t off) { return e.offset < off; });
    if (it != live_allocs_.end() && it->offset == offset)
        return it->handle;
    return VK_NULL_HANDLE;
}

uint64_t ArenaAllocator::nextPow2(uint64_t v)
{
    if (v == 0)
        return 1;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

} // namespace lux::render
