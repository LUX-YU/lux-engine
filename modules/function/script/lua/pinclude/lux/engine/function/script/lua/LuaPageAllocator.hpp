#pragma once
#include <lux/engine/function/script/lua/LuaAllocation.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace lux::script::lua
{
    // One VM, one owner thread. Only freed memory is reused; live Lua objects never move here.
    class LuaPageAllocator final
    {
        struct Page;
        struct alignas(std::max_align_t) Block final { Page* page; };
        struct alignas(std::max_align_t) Page final
        {
            Page* previous{};
            Page* next{};
            Page* available_previous{};
            Page* available_next{};
            void* free{};
            std::size_t index{}, stride{}, capacity{}, live{}, carved{};
        };
        struct alignas(std::max_align_t) Direct final
        {
            Direct* previous;
            Direct* next;
            std::size_t capacity;
            Block block;
        };
        static_assert(offsetof(Direct, block) + sizeof(Block) == sizeof(Direct));
        static constexpr std::size_t page_bytes_ = 64U * 1024U;
        static constexpr std::array<std::size_t, 8U> classes_{32U, 64U, 128U, 256U, 512U, 736U, 1536U, 4096U};
        static constexpr auto unlimited_ = (std::numeric_limits<std::size_t>::max)();
    public:
        using Allocate = void* (*)(void*, void*, std::size_t, std::size_t);
        explicit LuaPageAllocator(LuaVmConfiguration config) noexcept : budget_(config.idle_page_budget_bytes)
        {
            stats_.enabled = config.track_allocations;
        }
        ~LuaPageAllocator() { clear(); }
        LuaPageAllocator(const LuaPageAllocator&) = delete;
        LuaPageAllocator& operator=(const LuaPageAllocator&) = delete;
        [[nodiscard]] Allocate callback() const noexcept
        {
            return stats_.enabled ? &allocate<true> : &allocate<false>;
        }
        template<bool Track>
        static void* allocate(void* context, void* pointer, std::size_t old_size, std::size_t size) noexcept
        {
            return static_cast<LuaPageAllocator*>(context)->resize<Track>(pointer, old_size, size);
        }
        [[nodiscard]] LuaAllocationStats stats() const noexcept
        {
            if (!stats_.enabled) return {};
            auto result = stats_;
            result.idle_page_backing_bytes = idle_bytes_;
            std::size_t small_capacity{};
            for (auto* page = pages_; page; page = page->next)
            {
                if (page->live == 0U) continue;
                result.active_page_backing_bytes += page_bytes_;
                result.pinned_free_slot_bytes += (page->capacity - page->live) * classes_[page->index];
                small_capacity += page->live * classes_[page->index];
                result.metadata_and_header_bytes += page_bytes_ - page->capacity * classes_[page->index];
            }
            result.class_rounding_bytes = small_capacity - (result.live_bytes - result.large_requested_live_bytes);
            for (auto* block = direct_; block; block = block->next)
                result.large_block_backing_bytes += sizeof(Direct) + block->capacity;
            return result;
        }
        // Only idle pages can be released. A live page is never bulk-freed to conceal a missing Lua free.
        void clear() noexcept
        {
            while (idle_)
            {
                auto* page = idle_;
                removeAvailable(idle_, page);
                idle_bytes_ -= page_bytes_;
                if (stats_.enabled) destroyPage<true>(page); else destroyPage<false>(page);
            }
        }
        [[nodiscard]] bool hasLiveAllocations() const noexcept
        {
            if (direct_ != nullptr) return true;
            for (auto* page = pages_; page; page = page->next)
                if (page->live != 0U) return true;
            return false;
        }
        // Test-only lower-heap injection, distinct from the logical lua_Alloc fault wrapper.
        void failSystemAfter(std::size_t requests) noexcept { permitted_system_ = requests; }
        void failNextPage() noexcept { fail_next_page_ = true; }
    private:
        static void addAvailable(Page*& head, Page* page) noexcept
        {
            page->available_previous = nullptr;
            page->available_next = head;
            if (head) head->available_previous = page;
            head = page;
        }
        static void removeAvailable(Page*& head, Page* page) noexcept
        {
            if (page->available_previous) page->available_previous->available_next = page->available_next;
            else head = page->available_next;
            if (page->available_next) page->available_next->available_previous = page->available_previous;
            page->available_previous = page->available_next = nullptr;
        }
        template<bool Track> void* heapAllocate(std::size_t size) noexcept
        {
            if (permitted_system_ == 0U) return nullptr;
            if (permitted_system_ != unlimited_) --permitted_system_;
            if constexpr (Track) ++stats_.system_allocations;
            return std::malloc(size);
        }
        template<bool Track> void heapFree(void* pointer) noexcept
        {
            if constexpr (Track) ++stats_.system_frees;
            std::free(pointer);
        }
        template<bool Track> void destroyPage(Page* page) noexcept
        {
            if (page->previous) page->previous->next = page->next; else pages_ = page->next;
            if (page->next) page->next->previous = page->previous;
            if constexpr (Track) ++stats_.page_frees;
            heapFree<Track>(page);
        }
        template<bool Track> void* acquireDirect(std::size_t size) noexcept
        {
            if (size > unlimited_ - sizeof(Direct)) return nullptr;
            auto* direct = static_cast<Direct*>(heapAllocate<Track>(sizeof(Direct) + size));
            if (!direct) return nullptr;
            *direct = {nullptr, direct_, size, {nullptr}};
            if (direct_) direct_->previous = direct;
            direct_ = direct;
            if constexpr (Track) ++stats_.direct_allocations;
            return direct + 1;
        }
        template<bool Track> void* acquire(std::size_t size) noexcept
        {
            std::size_t index{};
            while (index < classes_.size() && size > classes_[index]) ++index;
            if (index == classes_.size()) return acquireDirect<Track>(size);
            auto* page = partial_[index];
            if (!page)
            {
                page = idle_;
                if (page)
                {
                    removeAvailable(idle_, page);
                    idle_bytes_ -= page_bytes_;
                    if constexpr (Track) ++stats_.page_reuses;
                }
                else
                {
                    if (fail_next_page_) fail_next_page_ = false;
                    else page = static_cast<Page*>(heapAllocate<Track>(page_bytes_));
                    if (!page)
                    {
                        if constexpr (Track) ++stats_.page_fallbacks;
                        return acquireDirect<Track>(size);
                    }
                    *page = {};
                    page->next = pages_;
                    if (pages_) pages_->previous = page;
                    pages_ = page;
                    if constexpr (Track) ++stats_.page_allocations;
                }
                page->index = index;
                page->stride = sizeof(Block) + classes_[index];
                page->capacity = (page_bytes_ - sizeof(Page)) / page->stride;
                page->live = page->carved = 0U;
                page->free = nullptr;
                addAvailable(partial_[index], page);
            }
            void* result = page->free;
            if (result)
            {
                page->free = *static_cast<void**>(result);
                if constexpr (Track) ++stats_.slot_reuses;
            }
            else
            {
                auto* bytes = reinterpret_cast<unsigned char*>(page) + sizeof(Page) + page->carved++ * page->stride;
                auto* block = reinterpret_cast<Block*>(bytes);
                block->page = page;
                result = block + 1;
            }
            if (++page->live == page->capacity) removeAvailable(partial_[index], page);
            return result;
        }
        static Direct* directBlock(void* pointer) noexcept { return static_cast<Direct*>(pointer) - 1; }
        static std::size_t capacity(void* pointer) noexcept
        {
            const auto* page = (static_cast<Block*>(pointer) - 1)->page;
            return page ? classes_[page->index] : directBlock(pointer)->capacity;
        }
        template<bool Track> void release(void* pointer) noexcept
        {
            auto* page = (static_cast<Block*>(pointer) - 1)->page;
            if (!page)
            {
                auto* block = directBlock(pointer);
                if (block->previous) block->previous->next = block->next; else direct_ = block->next;
                if (block->next) block->next->previous = block->previous;
                if constexpr (Track) ++stats_.direct_frees;
                heapFree<Track>(block);
                return;
            }
            if (page->live == page->capacity) addAvailable(partial_[page->index], page);
            *static_cast<void**>(pointer) = page->free;
            page->free = pointer;
            if (--page->live != 0U) return;
            removeAvailable(partial_[page->index], page);
            if (page_bytes_ > budget_ - idle_bytes_) destroyPage<Track>(page);
            else
            {
                addAvailable(idle_, page);
                idle_bytes_ += page_bytes_;
                if constexpr (Track)
                    stats_.peak_idle_page_backing_bytes = (std::max)(stats_.peak_idle_page_backing_bytes, idle_bytes_);
            }
        }
        template<bool Track> void* resize(void* pointer, std::size_t old_size, std::size_t size) noexcept
        {
            if (!pointer) old_size = 0U; // Lua supplies a type tag for first allocations.
            const bool old_direct = pointer && !(static_cast<Block*>(pointer) - 1)->page;
            if (size == 0U)
            {
                if (pointer)
                {
                    if constexpr (Track)
                    {
                        stats_.live_bytes -= old_size;
                        if (old_direct) stats_.large_requested_live_bytes -= old_size;
                        ++stats_.frees;
                        stats_.released_bytes += old_size;
                    }
                    release<Track>(pointer);
                }
                return nullptr;
            }
            void* result = pointer;
            if (pointer && size <= capacity(pointer))
            {
                if constexpr (Track) ++stats_.in_place;
            }
            else
            {
                result = acquire<Track>(size);
                if (!result)
                {
                    if constexpr (Track) ++stats_.failures;
                    return nullptr;
                }
                if (pointer)
                {
                    std::memcpy(result, pointer, (std::min)(old_size, size));
                    release<Track>(pointer);
                }
            }
            if constexpr (Track)
            {
                stats_.live_bytes = stats_.live_bytes - old_size + size;
                stats_.peak_live_bytes = (std::max)(stats_.peak_live_bytes, stats_.live_bytes);
                if (old_direct) stats_.large_requested_live_bytes -= old_size;
                if (!(static_cast<Block*>(result) - 1)->page) stats_.large_requested_live_bytes += size;
                if (pointer) ++stats_.reallocations; else ++stats_.allocations;
                stats_.requested_bytes += size;
                stats_.released_bytes += old_size;
            }
            return result;
        }
        std::array<Page*, classes_.size()> partial_{};
        Page* pages_{};
        Page* idle_{};
        Direct* direct_{};
        std::size_t budget_{}, idle_bytes_{};
        std::size_t permitted_system_{unlimited_};
        bool fail_next_page_{};
        LuaAllocationStats stats_{};
    };
}
