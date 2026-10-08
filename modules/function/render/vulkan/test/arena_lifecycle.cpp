#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/memory/ArenaAllocator.hpp>
#include <lux/engine/render/gpu/memory/ChainedArenaAllocator.hpp>

#include <cassert>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace
{
    bool fail_create{};
    unsigned created{}, destroyed{}, attempted{};
    std::unordered_set<VmaVirtualBlock> live;

    VkResult createBlock(const VmaVirtualBlockCreateInfo* info, VmaVirtualBlock* block)
    {
        ++attempted;
        if (fail_create)
        {
            *block = nullptr;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateVirtualBlock(info, block);
        if (result == VK_SUCCESS)
        {
            assert(live.insert(*block).second);
            ++created;
        }
        return result;
    }

    void destroyBlock(VmaVirtualBlock block)
    {
        assert(live.erase(block) == 1);
        vmaDestroyVirtualBlock(block);
        ++destroyed;
    }
} // namespace

// Compile the actual allocator algorithm, replacing only its native acquisition/release boundary.
// clang-format off
#define vmaCreateVirtualBlock createBlock
#define vmaDestroyVirtualBlock destroyBlock
#include "../src/gpu/memory/ArenaAllocator.cpp"
#undef vmaDestroyVirtualBlock
#undef vmaCreateVirtualBlock
// clang-format on

int main()
{
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<ArenaAllocator>);
    static_assert(std::is_nothrow_move_constructible_v<ArenaAllocator>);
    static_assert(std::is_nothrow_move_assignable_v<ArenaAllocator>);
    static_assert(std::is_nothrow_destructible_v<ArenaAllocator>);

    auto invalid = ArenaAllocator::create(0);
    assert(!invalid && isError<err::internal::InvalidArgument>(invalid.error()));
    assert(attempted == 0);

    for (unsigned iteration{}; iteration < 32; ++iteration)
    {
        ChainedArenaAllocator segments(2);
        fail_create = true;
        auto rejected = ArenaAllocator::create(4096);
        assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
        assert(segments.addSegment(4096) == ChainedArenaAllocator::kInvalidSegment);
        assert(segments.segmentCount() == 0 && segments.totalCapacity() == 0 && live.empty());

        fail_create = false;
        {
            auto first = ArenaAllocator::create(4096);
            auto second = ArenaAllocator::create(8192);
            assert(first && second && live.size() == 2);
            const auto a = first->allocate(64, 16);
            const auto b = first->allocate(32, 16);
            assert(a.valid() && b.valid());
            first->free(a);
            auto plan = first->planDefragmentation();
            assert(plan.size() == 1 && plan[0].src_offset == 64 && plan[0].dst_offset == 0);
            first->applyDefragmentation(plan);
            assert(first->usedBytes() == 32 && first->findHandleAt(0) != VK_NULL_HANDLE);
            assert(!first->allocate(1, 0).valid());
            assert(!first->allocate(std::numeric_limits<uint64_t>::max(), 3).valid());
            assert(!first->allocate(1, std::numeric_limits<uint64_t>::max()).valid());

            *second = std::move(*first);
            assert(live.size() == 1 && second->totalCapacity() == 4096 && second->usedBytes() == 32);
            assert(first->totalCapacity() == 0 && first->usedBytes() == 0 && !first->allocate(1).valid());
            ArenaAllocator moved(std::move(*second));
            assert(second->totalCapacity() == 0 && moved.usedBytes() == 32);
            moved = std::move(moved);
            assert(live.size() == 1 && moved.usedBytes() == 32);
        }
        assert(live.empty() && created == destroyed);

        assert(segments.addSegment(4096) == 0);
        const auto allocation = segments.allocate(64, 3);
        assert(allocation.valid() && allocation.offset % 3 == 0);
        fail_create = true;
        assert(segments.addSegment(4096) == ChainedArenaAllocator::kInvalidSegment);
        assert(segments.segmentCount() == 1 && segments.totalCapacity() == 4096);
        assert(segments.segment(0)->findHandleAt(allocation.offset) == allocation.handle);
        fail_create = false;
        assert(segments.addSegment(8192) == 1);
        const auto attempts = attempted;
        assert(segments.addSegment(1024) == ChainedArenaAllocator::kInvalidSegment && attempted == attempts);
        assert(segments.removeLastEmptySegment() && live.size() == 1);
        segments.free(allocation);
        assert(segments.totalUsedBytes() == 0);
        segments.clear();
        assert(live.empty() && created == destroyed);
    }
}
