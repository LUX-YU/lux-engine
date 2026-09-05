#pragma once

#include <lux/cxx/compile_time/expected.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace lux::simulation::script::detail
{
    struct StorageClassPlan final
    {
        std::size_t size{};
        std::size_t alignment{};
        std::size_t page_bytes{};
        std::size_t pages{};
    };

    // Explicit product provisioning for a uniform workload; not a heap fallback or a population estimate.
    [[nodiscard]] constexpr StorageClassPlan makeUniformStorageClass(
        std::size_t max_size, std::size_t alignment, std::size_t arena_bytes, std::size_t allocation_count
    ) noexcept
    {
        if (allocation_count == 0U || alignment == 0U)
            return {};
        const auto available = arena_bytes / allocation_count;
        const auto size = (std::min)(max_size, available - available % alignment);
        return {size, alignment, arena_bytes, 1U};
    }

    enum class EClassStorageError : std::uint8_t
    {
        INVALID_CONFIGURATION,
        CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
    };

    // Storage only: the containing backend owns its budget and serializes preparation/maintenance.
    class BoundedClassStorage final
    {
        static constexpr auto Invalid = (std::numeric_limits<std::uint32_t>::max)();

    public:
        struct ClassHandle final
        {
            std::uint32_t index{Invalid};
            [[nodiscard]] explicit operator bool() const noexcept { return index != Invalid; }
        };

        struct Allocation final
        {
            void* data{};
            std::uint32_t page{Invalid};
            std::uint32_t slot{Invalid};
            std::uint64_t generation{};
            std::size_t size{};
            [[nodiscard]] explicit operator bool() const noexcept { return data != nullptr; }
        };

        struct Stats final
        {
            std::size_t arena_bytes{};
            std::size_t metadata_bytes{};
            std::size_t active_allocations{};
            std::size_t allocation_high_water{};
            std::size_t live_bytes{};
            std::size_t occupied_bytes{};
            std::size_t capacity_failures{};
            std::uint64_t selection_steps{};
            std::uint64_t acquire_steps{};
            std::uint64_t release_steps{};
            std::uint64_t maintenance_steps{};
        };

        [[nodiscard]] static lux::cxx::expected<BoundedClassStorage, EClassStorageError> create(
            std::span<const StorageClassPlan> plans, std::size_t max_bytes, std::size_t allocation_capacity,
            std::uint64_t generation_limit = (std::numeric_limits<std::uint64_t>::max)()
        ) noexcept
        {
            const bool is_invalid_input = plans.empty() || plans.size() > 64U || allocation_capacity == 0U ||
                allocation_capacity >= Invalid || max_bytes == 0U;
            if (is_invalid_input)
                return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
            std::size_t max_alignment{alignof(std::max_align_t)};
            std::size_t minimum_stride{(std::numeric_limits<std::size_t>::max)()};
            for (const auto& plan : plans)
            {
                const bool is_invalid_layout = plan.size == 0U || !powerOfTwo(plan.alignment) ||
                    plan.size > (std::numeric_limits<std::size_t>::max)() - (plan.alignment - 1U);
                if (is_invalid_layout)
                    return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
                const auto stride = alignUp(plan.size, plan.alignment);
                if (plan.page_bytes < stride || plan.pages >= Invalid)
                    return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
                max_alignment = (std::max)(max_alignment, plan.alignment);
                minimum_stride = (std::min)(minimum_stride, stride);
            }
            std::size_t arena_bytes{};
            std::size_t page_count{};
            std::size_t slot_count{};
            for (const auto& plan : plans)
            {
                const bool is_invalid_page = plan.page_bytes > (std::numeric_limits<std::size_t>::max)() -
                    (max_alignment - 1U);
                if (is_invalid_page)
                    return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
                const auto page_stride = alignUp(plan.page_bytes, max_alignment);
                const auto page_slots = plan.page_bytes / minimum_stride;
                const bool exceeds_budget = plan.pages > (max_bytes - arena_bytes) / page_stride;
                const bool exceeds_indices = plan.pages > Invalid - 1U - page_count ||
                    page_slots >= Invalid || plan.pages > (Invalid - 1U - slot_count) / page_slots;
                if (exceeds_budget || exceeds_indices)
                    return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
                arena_bytes += page_stride * plan.pages;
                page_count += plan.pages;
                slot_count += page_slots * plan.pages;
            }
            if (arena_bytes == 0U || arena_bytes > static_cast<std::size_t>(
                    (std::numeric_limits<std::ptrdiff_t>::max)()))
                return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
            auto remaining = max_bytes - arena_bytes;
            if (remaining < sizeof(BoundedClassStorage))
                return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
            remaining -= sizeof(BoundedClassStorage);
            if (plans.size() > remaining / sizeof(Class))
                return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
            remaining -= plans.size() * sizeof(Class);
            if (page_count > remaining / sizeof(Page))
                return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
            remaining -= page_count * sizeof(Page);
            if (slot_count > remaining / sizeof(Slot))
                return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);

            BoundedClassStorage result;
            result.arena_.alignment = max_alignment;
            result.arena_.data = ::operator new(arena_bytes, std::align_val_t{max_alignment}, std::nothrow);
            if (result.arena_.data == nullptr)
                return lux::cxx::unexpected(EClassStorageError::ALLOCATION_FAILURE);
            try
            {
                result.classes_.resize(plans.size());
                result.pages_.resize(page_count);
                result.slots_.resize(slot_count);
                result.capacity_ = allocation_capacity;
                result.generation_limit_ = generation_limit;
                result.stats_.arena_bytes = arena_bytes;
                result.stats_.metadata_bytes = sizeof(BoundedClassStorage) +
                    result.classes_.capacity() * sizeof(Class) + result.pages_.capacity() * sizeof(Page) +
                    result.slots_.capacity() * sizeof(Slot);
                if (result.stats_.metadata_bytes > max_bytes - arena_bytes)
                    return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
                std::uint32_t next_page{};
                std::uint32_t next_slot{};
                std::size_t offset{};
                for (std::size_t index{}; index < plans.size(); ++index)
                {
                    const auto& plan = plans[index];
                    auto& prepared = result.classes_[index];
                    prepared.size = plan.size;
                    prepared.alignment = plan.alignment;
                    prepared.stride = alignUp(plan.size, plan.alignment);
                    for (std::size_t count{}; count < plan.pages; ++count)
                    {
                        auto& page = result.pages_[next_page];
                        page.offset = offset;
                        page.bytes = plan.page_bytes;
                        page.metadata_first = next_slot;
                        page.metadata_count = static_cast<std::uint32_t>(plan.page_bytes / minimum_stride);
                        page.class_index = static_cast<std::uint32_t>(index);
                        result.initializePage(next_page);
                        next_slot += page.metadata_count;
                        offset += alignUp(plan.page_bytes, max_alignment);
                        ++next_page;
                    }
                }
                result.stats_.maintenance_steps = 0U;
                return result;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EClassStorageError::ALLOCATION_FAILURE);
            }
        }

        BoundedClassStorage() noexcept = default;
        BoundedClassStorage(const BoundedClassStorage&) = delete;
        BoundedClassStorage& operator=(const BoundedClassStorage&) = delete;
        BoundedClassStorage(BoundedClassStorage&&) noexcept = default;
        BoundedClassStorage& operator=(BoundedClassStorage&&) noexcept = default;

        [[nodiscard]] ClassHandle select(std::size_t size, std::size_t alignment) noexcept
        {
            if (size == 0U || !powerOfTwo(alignment))
                return {};
            ClassHandle best;
            for (std::uint32_t index{}; index < classes_.size(); ++index)
            {
                ++stats_.selection_steps;
                const auto& candidate = classes_[index];
                const bool fits = candidate.size >= size && candidate.alignment >= alignment;
                if (fits && (!best || candidate.stride < classes_[best.index].stride))
                    best.index = index;
            }
            return best;
        }

        [[nodiscard]] lux::cxx::expected<Allocation, EClassStorageError> acquire(
            ClassHandle handle, std::size_t size
        ) noexcept
        {
            const bool is_invalid_class = handle.index >= classes_.size();
            if (is_invalid_class || size == 0U || size > classes_[handle.index].size ||
                stats_.active_allocations >= capacity_ || next_generation_ == generation_limit_)
                return fail();
            auto& prepared = classes_[handle.index];
            const auto page_index = prepared.nonfull;
            if (page_index == Invalid)
                return fail();
            auto& page = pages_[page_index];
            const auto local_slot = page.free_head;
            auto& slot = slots_[page.metadata_first + local_slot];
            stats_.acquire_steps += 3U;
            page.free_head = slot.next;
            if (page.free_head == Invalid)
                stats_.acquire_steps += unlinkPage(page_index);
            slot.active = true;
            slot.size = size;
            slot.generation = ++next_generation_;
            ++page.active;
            ++stats_.active_allocations;
            stats_.allocation_high_water = (std::max)(stats_.allocation_high_water, stats_.active_allocations);
            stats_.live_bytes += size;
            stats_.occupied_bytes += prepared.stride;
            return Allocation{
                static_cast<std::byte*>(arena_.data) + page.offset + prepared.stride * local_slot,
                page_index, local_slot, slot.generation, size
            };
        }

        [[nodiscard]] bool release(Allocation allocation) noexcept
        {
            if (!allocation || allocation.page >= pages_.size())
                return false;
            auto& page = pages_[allocation.page];
            if (allocation.slot >= page.slot_count)
                return false;
            auto& slot = slots_[page.metadata_first + allocation.slot];
            const auto& prepared = classes_[page.class_index];
            const auto* expected = static_cast<std::byte*>(arena_.data) + page.offset +
                prepared.stride * allocation.slot;
            const bool is_invalid_slot = !slot.active || slot.generation != allocation.generation ||
                slot.size != allocation.size || allocation.data != expected;
            if (is_invalid_slot)
                return false;
            stats_.release_steps += 3U;
            slot.active = false;
            --page.active;
            --stats_.active_allocations;
            stats_.live_bytes -= slot.size;
            stats_.occupied_bytes -= prepared.stride;
            slot.size = 0U;
            if (page.free_head == Invalid)
                stats_.release_steps += linkPage(allocation.page);
            slot.next = page.free_head;
            page.free_head = allocation.slot;
            return true;
        }

        // Explicit cold maintenance. Other pages may be live; no address or interpretation on them changes.
        [[nodiscard]] bool reclassifyEmptyPage(std::uint32_t page_index, ClassHandle target) noexcept
        {
            if (page_index >= pages_.size() || target.index >= classes_.size())
                return false;
            auto& page = pages_[page_index];
            const auto count = page.bytes / classes_[target.index].stride;
            const bool is_invalid_page = page.active != 0U || count == 0U || count > page.metadata_count;
            if (is_invalid_page)
                return false;
            if (page.free_head != Invalid)
                stats_.maintenance_steps += unlinkPage(page_index);
            page.class_index = target.index;
            initializePage(page_index);
            return true;
        }

        [[nodiscard]] Stats stats() const noexcept { return stats_; }

    private:
        struct Arena final
        {
            void* data{};
            std::size_t alignment{alignof(std::max_align_t)};
            Arena() noexcept = default;
            Arena(const Arena&) = delete;
            Arena& operator=(const Arena&) = delete;
            Arena(Arena&& other) noexcept : data(std::exchange(other.data, nullptr)), alignment(other.alignment) {}
            Arena& operator=(Arena&& other) noexcept
            {
                if (this != &other)
                {
                    if (data != nullptr)
                        ::operator delete(data, std::align_val_t{alignment});
                    data = std::exchange(other.data, nullptr);
                    alignment = other.alignment;
                }
                return *this;
            }
            ~Arena() noexcept
            {
                if (data != nullptr)
                    ::operator delete(data, std::align_val_t{alignment});
            }
        };
        struct Class final
        {
            std::size_t size{};
            std::size_t alignment{};
            std::size_t stride{};
            std::uint32_t nonfull{Invalid};
        };
        struct Page final
        {
            std::size_t offset{};
            std::size_t bytes{};
            std::uint32_t class_index{};
            std::uint32_t metadata_first{};
            std::uint32_t metadata_count{};
            std::uint32_t slot_count{};
            std::uint32_t free_head{Invalid};
            std::uint32_t previous{Invalid};
            std::uint32_t next{Invalid};
            std::uint32_t active{};
        };
        struct Slot final
        {
            std::uint64_t generation{};
            std::size_t size{};
            std::uint32_t next{Invalid};
            bool active{};
        };

        [[nodiscard]] static bool powerOfTwo(std::size_t value) noexcept
        {
            return value != 0U && (value & (value - 1U)) == 0U;
        }
        [[nodiscard]] static std::size_t alignUp(std::size_t size, std::size_t alignment) noexcept
        {
            return (size + alignment - 1U) & ~(alignment - 1U);
        }
        void initializePage(std::uint32_t index) noexcept
        {
            auto& page = pages_[index];
            page.slot_count = static_cast<std::uint32_t>(page.bytes / classes_[page.class_index].stride);
            for (std::uint32_t slot{}; slot < page.slot_count; ++slot)
            {
                slots_[page.metadata_first + slot] = Slot{0U, 0U, slot + 1U, false};
                ++stats_.maintenance_steps;
            }
            slots_[page.metadata_first + page.slot_count - 1U].next = Invalid;
            page.free_head = 0U;
            stats_.maintenance_steps += linkPage(index);
        }
        [[nodiscard]] std::uint64_t linkPage(std::uint32_t index) noexcept
        {
            auto& page = pages_[index];
            auto& prepared = classes_[page.class_index];
            page.previous = Invalid;
            page.next = prepared.nonfull;
            if (prepared.nonfull != Invalid)
                pages_[prepared.nonfull].previous = index;
            prepared.nonfull = index;
            return 2U + static_cast<std::uint64_t>(page.next != Invalid);
        }
        [[nodiscard]] std::uint64_t unlinkPage(std::uint32_t index) noexcept
        {
            auto& page = pages_[index];
            if (page.previous != Invalid)
                pages_[page.previous].next = page.next;
            else
                classes_[page.class_index].nonfull = page.next;
            if (page.next != Invalid)
                pages_[page.next].previous = page.previous;
            const auto visits = 2U + static_cast<std::uint64_t>(page.next != Invalid);
            page.previous = Invalid;
            page.next = Invalid;
            return visits;
        }
        [[nodiscard]] lux::cxx::expected<Allocation, EClassStorageError> fail() noexcept
        {
            ++stats_.capacity_failures;
            return lux::cxx::unexpected(EClassStorageError::CAPACITY_EXCEEDED);
        }

        Arena arena_;
        std::vector<Class> classes_;
        std::vector<Page> pages_;
        std::vector<Slot> slots_;
        std::size_t capacity_{};
        std::uint64_t next_generation_{};
        std::uint64_t generation_limit_{(std::numeric_limits<std::uint64_t>::max)()};
        Stats stats_;
    };
}
