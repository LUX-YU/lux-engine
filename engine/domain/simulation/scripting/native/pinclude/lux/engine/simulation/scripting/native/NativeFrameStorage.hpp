#pragma once
#include <lux/engine/simulation/scripting/detail/BoundedClassStorage.hpp>
#include <cstring>

namespace lux::simulation::script::detail
{
    // Each original envelope owns N regions of R bytes. Equal envelopes share one domain.
    // k < N live frames imply at most k nonempty regions: any next legal frame has a region.
    class NativeFrameStorage final
    {
        static constexpr std::uint32_t Invalid = UINT32_MAX;
        static constexpr std::size_t Maximum = (std::numeric_limits<std::size_t>::max)();
    public:
        struct Layout final
        {
            std::uint32_t index{Invalid};
            [[nodiscard]] explicit operator bool() const noexcept { return index != Invalid; }
        };
        struct Ticket final
        {
            const NativeFrameStorage* owner{};
            std::uint64_t generation{};
            std::uint32_t region{Invalid}, slot{Invalid};
        };
        // The live identity resides in the existing NativeContinuation, never in a second slot directory.
        // Copies of Ticket cannot become the authoritative Lease used to release a region slot.
        class Lease final
        {
            friend class NativeFrameStorage;
            Ticket identity_;
        public:
            void* data{};
            Lease() noexcept = default;
            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;
            Lease& operator=(Lease&&) = delete;
            Lease(Lease&& other) noexcept : identity_(std::exchange(other.identity_, {})),
                data(std::exchange(other.data, nullptr)) {}
            [[nodiscard]] Ticket ticket() const noexcept { return identity_; }
            [[nodiscard]] explicit operator bool() const noexcept { return identity_.owner != nullptr; }
        };
        struct Stats final
        {
            bool observation_collected{};
            std::size_t arena_bytes{}, metadata_bytes{}, reserved_slots{}, active_allocations{};
            std::size_t allocation_high_water{}, live_bytes{}, occupied_bytes{}, capacity_failures{};
            std::size_t active_region_bytes{};
            std::uint64_t acquire_steps{}, release_steps{};
        };
        [[nodiscard]] static lux::cxx::expected<NativeFrameStorage, EClassStorageError> create(
            std::span<const StorageClassPlan> plans, std::size_t budget, std::size_t capacity,
            std::size_t layout_capacity, bool observe = false, std::uint64_t generation_limit = UINT64_MAX
        ) noexcept
        {
            const bool invalid = plans.empty() || plans.size() > 64U || capacity == 0U || capacity >= Invalid ||
                layout_capacity == 0U || layout_capacity >= Invalid;
            if (invalid) return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
            std::size_t alignment = alignof(std::max_align_t);
            for (const auto& p : plans)
            {
                const bool invalid_shape = p.size == 0U || p.size > UINT32_MAX || !powerOfTwo(p.alignment) ||
                    p.size > Maximum - (p.alignment - 1U) || p.pages != 1U;
                if (invalid_shape) return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
                alignment = (std::max)(alignment, p.alignment);
            }
            std::size_t bytes{}, regions{};
            for (const auto& p : plans)
            {
                const auto stride = alignUp(p.size, p.alignment);
                const bool invalid_domain = p.page_bytes == 0U || p.page_bytes % stride != 0U ||
                    p.page_bytes > Maximum - (alignment - 1U);
                if (invalid_domain) return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
                const auto count = p.page_bytes / stride;
                const auto backing = alignUp(p.page_bytes, alignment);
                if (count > capacity - regions || backing > budget - bytes)
                    return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
                regions += count;
                bytes += backing;
            }
            auto remaining = budget - bytes;
            const auto consume = [&](std::size_t count, std::size_t size) noexcept {
                if (count > remaining / size) return false;
                remaining -= count * size;
                return true;
            };
            // Charge the embedded tickets too, even though the backend supplies their physical storage.
            if (!consume(1U, sizeof(NativeFrameStorage)) || !consume(plans.size(), sizeof(Domain)) ||
                !consume(regions, sizeof(Region)) || !consume(layout_capacity, sizeof(Class)) ||
                !consume(capacity, sizeof(Lease)))
                return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
            NativeFrameStorage result;
            result.arena_.alignment = alignment;
            result.arena_.data = ::operator new(bytes, std::align_val_t{alignment}, std::nothrow);
            if (!result.arena_.data) return lux::cxx::unexpected(EClassStorageError::ALLOCATION_FAILURE);
            try
            {
                result.domains_.resize(plans.size());
                result.regions_.resize(regions);
                result.classes_.resize(layout_capacity);
                result.capacity_ = capacity;
                result.generation_limit_ = generation_limit;
                result.stats_.observation_collected = observe;
                result.stats_.arena_bytes = bytes;
                result.stats_.reserved_slots = regions;
                result.stats_.metadata_bytes = sizeof(NativeFrameStorage) + result.domains_.capacity() * sizeof(Domain) +
                    result.regions_.capacity() * sizeof(Region) + result.classes_.capacity() * sizeof(Class) +
                    capacity * sizeof(Lease);
                if (result.stats_.metadata_bytes > budget - bytes)
                    return lux::cxx::unexpected(EClassStorageError::INVALID_CONFIGURATION);
                std::uint32_t first{};
                std::size_t offset{};
                for (std::uint32_t i{}; i < plans.size(); ++i)
                {
                    const auto& p = plans[i];
                    const auto stride = alignUp(p.size, p.alignment);
                    const auto count = static_cast<std::uint32_t>(p.page_bytes / stride);
                    result.domains_[i] = {p.size, p.alignment, stride, offset, first, count, first, 0U};
                    for (std::uint32_t n{}; n < count; ++n)
                        result.regions_[first + n].next = n + 1U == count ? Invalid : first + n + 1U;
                    first += count;
                    offset += alignUp(p.page_bytes, alignment);
                }
                for (std::uint32_t i{}; i < layout_capacity; ++i)
                    result.classes_[i].nonfull = i + 1U == layout_capacity ? Invalid : i + 1U;
                result.free_class_ = 0U;
                return result;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EClassStorageError::ALLOCATION_FAILURE);
            }
        }
        NativeFrameStorage() noexcept = default;
        NativeFrameStorage(const NativeFrameStorage&) = delete;
        NativeFrameStorage& operator=(const NativeFrameStorage&) = delete;
        NativeFrameStorage(NativeFrameStorage&&) noexcept = default;
        NativeFrameStorage& operator=(NativeFrameStorage&&) noexcept = default;

        [[nodiscard]] Layout domain(std::size_t size, std::size_t alignment) const noexcept
        {
            for (std::uint32_t i{}; i < domains_.size(); ++i)
                if (domains_[i].size == size && domains_[i].alignment == alignment) return {i};
            return {};
        }
        [[nodiscard]] Layout prepare(Layout domain, std::size_t size, std::size_t alignment) noexcept
        {
            if (domain.index >= domains_.size() || !powerOfTwo(alignment)) return {};
            const auto& d = domains_[domain.index];
            if (size == 0U || size > d.size || alignment > d.alignment || size > Maximum - alignment + 1U) return {};
            for (std::uint32_t i{}; i < classes_.size(); ++i)
            {
                auto& c = classes_[i];
                if (c.references && c.domain == domain.index && c.size == size && c.alignment == alignment)
                {
                    if (c.references == Invalid) return {};
                    ++c.references;
                    return {i};
                }
            }
            if (free_class_ == Invalid) return {};
            const auto index = free_class_;
            auto& c = classes_[index];
            free_class_ = c.nonfull;
            auto stride = alignUp(size, alignment);
            if (stride < sizeof(std::uint32_t)) stride = d.stride; // One tiny frame per guarantee region.
            c = {stride, static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(alignment), domain.index,
                Invalid, 1U, 0U};
            return {index};
        }
        [[nodiscard]] bool releaseLayout(Layout layout) noexcept
        {
            if (layout.index >= classes_.size()) return false;
            auto& c = classes_[layout.index];
            if (!c.references || (c.references == 1U && c.live != 0U)) return false;
            if (--c.references == 0U)
            {
                c.nonfull = free_class_;
                free_class_ = layout.index;
            }
            return true;
        }
        [[nodiscard]] bool acquire(Layout layout, Lease& lease) noexcept
        {
            if (lease || layout.index >= classes_.size() || active_ == capacity_ || generation_ == generation_limit_)
                return fail();
            auto& c = classes_[layout.index];
            if (!c.references) return fail();
            auto& d = domains_[c.domain];
            if (d.active == d.count) return fail();
            auto index = c.nonfull;
            if (index == Invalid)
            {
                index = d.empty;
                if (index == Invalid) return fail();
                auto& r = regions_[index];
                d.empty = r.next;
                r = {layout.index, 0U, 0U, Invalid, Invalid, Invalid};
                link(index);
                if (stats_.observation_collected) stats_.active_region_bytes += d.stride;
            }
            auto& r = regions_[index];
            const auto slot = r.free == Invalid ? r.carved++ : r.free;
            void* data = pointer(d, c, index, slot);
            if (r.free != Invalid) std::memcpy(&r.free, data, sizeof(r.free));
            if (++r.live == d.stride / c.stride) unlink(index);
            ++active_;
            ++d.active;
            ++c.live;
            lease.identity_ = {this, ++generation_, index, slot};
            lease.data = data;
            if (stats_.observation_collected)
            {
                ++stats_.acquire_steps;
                stats_.live_bytes += c.size;
                stats_.occupied_bytes += c.stride;
                stats_.allocation_high_water = (std::max)(stats_.allocation_high_water, active_);
            }
            return true;
        }
        [[nodiscard]] bool release(Lease& lease, Ticket ticket) noexcept
        {
            const auto& live = lease.identity_;
            const bool matches = ticket.owner == this && live.owner == this && ticket.generation == live.generation &&
                ticket.region == live.region && ticket.slot == live.slot;
            if (!matches || live.region >= regions_.size()) return false;
            auto& r = regions_[live.region];
            if (r.layout >= classes_.size()) return false;
            auto& c = classes_[r.layout];
            auto& d = domains_[c.domain];
            const auto slots = d.stride / c.stride;
            if (!r.live || live.slot >= r.carved || lease.data != pointer(d, c, live.region, live.slot)) return false;
            if (r.live == slots) link(live.region);
            --r.live;
            --c.live;
            --d.active;
            --active_;
            if (r.live == 0U)
            {
                unlink(live.region);
                r.layout = Invalid;
                r.next = d.empty;
                d.empty = live.region;
                if (stats_.observation_collected) stats_.active_region_bytes -= d.stride;
            }
            else
            {
                std::memcpy(lease.data, &r.free, sizeof(r.free));
                r.free = live.slot;
            }
            if (stats_.observation_collected)
            {
                ++stats_.release_steps;
                stats_.live_bytes -= c.size;
                stats_.occupied_bytes -= c.stride;
            }
            lease.identity_ = {};
            lease.data = nullptr;
            return true;
        }
        [[nodiscard]] bool release(Lease& lease) noexcept { return release(lease, lease.ticket()); }
        [[nodiscard]] Stats stats() const noexcept
        {
            auto result = stats_;
            result.active_allocations = active_;
            return result;
        }
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
                    if (data) ::operator delete(data, std::align_val_t{alignment});
                    data = std::exchange(other.data, nullptr);
                    alignment = other.alignment;
                }
                return *this;
            }
            ~Arena() { if (data) ::operator delete(data, std::align_val_t{alignment}); }
        } arena_;
        struct Domain final
        {
            std::size_t size{}, alignment{}, stride{}, offset{};
            std::uint32_t first{}, count{}, empty{Invalid}, active{};
        };
        struct Class final
        {
            std::size_t stride{};
            std::uint32_t size{}, alignment{}, domain{}, nonfull{Invalid}, references{}, live{};
        };
        struct Region final
        {
            std::uint32_t layout{Invalid}, live{}, carved{}, free{Invalid}, previous{Invalid}, next{Invalid};
        };
        [[nodiscard]] static bool powerOfTwo(std::size_t n) noexcept { return n && !(n & (n - 1U)); }
        [[nodiscard]] static std::size_t alignUp(std::size_t n, std::size_t a) noexcept { return (n + a - 1U) & ~(a - 1U); }
        [[nodiscard]] void* pointer(const Domain& d, const Class& c, std::uint32_t region, std::uint32_t slot) noexcept
        {
            return static_cast<std::byte*>(arena_.data) + d.offset + (region - d.first) * d.stride + slot * c.stride;
        }
        void link(std::uint32_t index) noexcept
        {
            auto& r = regions_[index];
            auto& head = classes_[r.layout].nonfull;
            r.previous = Invalid;
            r.next = head;
            if (head != Invalid) regions_[head].previous = index;
            head = index;
        }
        void unlink(std::uint32_t index) noexcept
        {
            auto& r = regions_[index];
            if (r.previous != Invalid) regions_[r.previous].next = r.next;
            else classes_[r.layout].nonfull = r.next;
            if (r.next != Invalid) regions_[r.next].previous = r.previous;
            r.previous = r.next = Invalid;
        }
        [[nodiscard]] bool fail() noexcept { ++stats_.capacity_failures; return false; }
        std::vector<Domain> domains_;
        std::vector<Class> classes_;
        std::vector<Region> regions_;
        std::uint32_t free_class_{Invalid};
        std::size_t capacity_{}, active_{};
        std::uint64_t generation_{}, generation_limit_{UINT64_MAX};
        Stats stats_;
    };
}
