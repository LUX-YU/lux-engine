#pragma once
#include <lux/engine/function/script/lua/LuaAllocation.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace lux::script::lua
{
    // Only VM-freed memory enters these lists. No live thread/object is reused.
    class LuaAllocationCache final
    {
        struct alignas(std::max_align_t) Block final
        {
            std::size_t capacity, requested, index;
            Block* next;
        };
        // Observed Lua55 coroutine requests: 64 (CallInfo), 216/240 (official/patched thread), 720 (stack).
        static constexpr std::array<std::size_t, 8U> classes_{32U, 64U, 128U, 256U, 512U, 736U, 1536U, 4096U};
    public:
        explicit LuaAllocationCache(LuaVmConfiguration config) noexcept : budget_(config.cache_bytes)
        {
            stats_.enabled = config.track_allocations;
        }
        ~LuaAllocationCache() { clear(); }
        LuaAllocationCache(const LuaAllocationCache&) = delete;
        LuaAllocationCache& operator=(const LuaAllocationCache&) = delete;
        [[nodiscard]] LuaAllocationStats stats() const noexcept { return stats_; }
        // Diagnostic injection is deliberately below the logical Lua request layer.
        void failSystemAfter(std::size_t requests) noexcept { permitted_system_ = requests; }
        void clear() noexcept
        {
            for (auto& head : free_)
                while (head)
                {
                    auto* block = head;
                    head = head->next;
                    std::free(block);
                    ++stats_.system_frees;
                }
            stats_.retained_bytes = 0U;
        }
        static void* allocate(void* context, void* pointer, std::size_t, std::size_t size) noexcept
        {
            return static_cast<LuaAllocationCache*>(context)->resize(pointer, size);
        }
    private:
        Block* acquire(std::size_t size) noexcept
        {
            std::size_t index{};
            while (index < classes_.size() && size > classes_[index]) ++index;
            const auto capacity = index < classes_.size() ? classes_[index] : size;
            if (index < classes_.size() && free_[index])
            {
                auto* block = free_[index];
                free_[index] = block->next;
                stats_.retained_bytes -= sizeof(Block) + block->capacity;
                ++stats_.cache_hits;
                return block;
            }
            if (capacity > (std::numeric_limits<std::size_t>::max)() - sizeof(Block)) return nullptr;
            if (permitted_system_ == 0U) return nullptr;
            if (permitted_system_ != (std::numeric_limits<std::size_t>::max)()) --permitted_system_;
            auto* block = static_cast<Block*>(std::malloc(sizeof(Block) + capacity));
            if (!block) return nullptr;
            *block = {capacity, size, index, nullptr};
            ++stats_.system_allocations;
            return block;
        }
        void release(Block* block) noexcept
        {
            const auto bytes = sizeof(Block) + block->capacity;
            const bool fits = block->index < classes_.size() && bytes <= budget_ - stats_.retained_bytes;
            if (fits)
            {
                block->next = free_[block->index];
                free_[block->index] = block;
                stats_.retained_bytes += bytes;
                stats_.peak_retained_bytes = (std::max)(stats_.peak_retained_bytes, stats_.retained_bytes);
            }
            else
            {
                std::free(block);
                ++stats_.system_frees;
            }
        }
        void* resize(void* pointer, std::size_t size) noexcept
        {
            auto* old = pointer ? static_cast<Block*>(pointer) - 1 : nullptr;
            const auto old_size = old ? old->requested : 0U;
            if (size == 0U)
            {
                if (old)
                {
                    stats_.live_bytes -= old_size;
                    if (stats_.enabled) { ++stats_.frees; stats_.released_bytes += old_size; }
                    release(old);
                }
                return nullptr;
            }
            Block* block = old;
            if (old && size <= old->capacity) ++stats_.in_place;
            else
            {
                block = acquire(size);
                if (!block) { ++stats_.failures; return nullptr; }
                if (old)
                {
                    std::memcpy(block + 1, old + 1, (std::min)(old_size, size));
                    release(old);
                }
            }
            block->requested = size;
            stats_.live_bytes = stats_.live_bytes - old_size + size;
            stats_.peak_live_bytes = (std::max)(stats_.peak_live_bytes, stats_.live_bytes);
            if (stats_.enabled)
            {
                if (pointer) ++stats_.reallocations; else ++stats_.allocations;
                stats_.requested_bytes += size;
                stats_.released_bytes += old_size;
            }
            return block + 1;
        }
        std::array<Block*, classes_.size()> free_{};
        std::size_t budget_{};
        std::size_t permitted_system_{(std::numeric_limits<std::size_t>::max)()};
        LuaAllocationStats stats_;
    };
}
