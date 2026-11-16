#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <lux/engine/render/extensions/resource/ResourceBinding.hpp>
#include <type_traits>
#include <vector>

namespace lux::render::detail
{
    // Fixed cold capacity. One CPU authority; each physical slice has its own dirty set and
    // immutable prepared bytes. These CPU snapshots are not GPU staging-buffer owners.
    template <class T> class ReplicatedData final
    {
        static_assert(std::is_trivially_copyable_v<T> && std::is_nothrow_default_constructible_v<T>);

        struct Slice final
        {
            ReplicatedData* owner;
            std::vector<std::uint8_t> marked;
            std::vector<std::uint32_t> dirty, staged;
            std::vector<std::byte> frozen;
            std::vector<CopyRange> copies;
            bool preparing = false;

            Slice(ReplicatedData& data, std::uint32_t count) noexcept
                : owner(&data), marked(count, 1), frozen(data.requiredUploadBytes())
            {
                dirty.reserve(count);
                staged.reserve(count);
                copies.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    dirty.push_back(i);
                }
            }

            void mark(std::uint32_t index) noexcept
            {
                if (!marked[index])
                {
                    marked[index] = 1;
                    dirty.push_back(index);
                }
            }

            static void finish(void* value, EPreparationOutcome outcome) noexcept
            {
                auto& slice = *static_cast<Slice*>(value);
                if (!slice.preparing)
                {
                    std::terminate();
                }
                slice.preparing = false;
                if (outcome == EPreparationOutcome::UNKNOWN)
                {
                    slice.owner->quarantined_ = true;
                    return; // Keep the frozen attempt; never turn uncertain native work into a retry.
                }
                if (outcome == EPreparationOutcome::REJECTED)
                {
                    for (auto index : slice.staged)
                    {
                        slice.mark(index);
                    }
                }
                slice.staged.clear();
                slice.copies.clear();
            }
        };

    public:
        ReplicatedData(std::uint32_t records, std::uint32_t frames_in_flight) noexcept
        {
            const bool is_invalid_capacity = !frames_in_flight || records > SIZE_MAX / sizeof(T);
            if (is_invalid_capacity)
            {
                std::terminate();
            }
            values_.resize(records);
            slices_.reserve(frames_in_flight);
            for (std::uint32_t i = 0; i < frames_in_flight; ++i)
            {
                slices_.emplace_back(*this, records);
            }
        }

        ~ReplicatedData() noexcept
        {
            for (const auto& slice : slices_)
            {
                if (slice.preparing)
                {
                    std::terminate();
                }
            }
        }

        ReplicatedData(const ReplicatedData&) = delete;
        ReplicatedData& operator=(const ReplicatedData&) = delete;

        [[nodiscard]] std::size_t requiredUploadBytes() const noexcept
        {
            return values_.size() * sizeof(T);
        }

        [[nodiscard]] bool quarantined() const noexcept
        {
            return quarantined_;
        }

        [[nodiscard]] RenderResult<T> value(std::uint32_t index) const noexcept
        {
            if (index >= values_.size())
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            return values_[index];
        }

        [[nodiscard]] RenderResult<void> update(std::uint32_t index, const T& value) noexcept
        {
            if (quarantined_)
            {
                return cxx::unexpected(RenderError{kOutcomeUnknown});
            }
            if (index >= values_.size())
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            std::memcpy(&values_[index], &value, sizeof(T));
            for (auto& slice : slices_)
            {
                slice.mark(index);
            }
            return {};
        }

        [[nodiscard]] RenderResult<PreparationCapacity> requirements(std::uint32_t slot) const noexcept
        {
            if (quarantined_)
            {
                return cxx::unexpected(RenderError{kOutcomeUnknown});
            }
            if (slot >= slices_.size())
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            const auto& slice = slices_[slot];
            if (slice.preparing)
            {
                return cxx::unexpected(RenderError{kDeviceBusy});
            }
            const auto bytes = slice.dirty.empty() ? 0 : requiredUploadBytes();
            // One range per dirty record is a conservative, allocation-free upper bound.
            return PreparationCapacity{bytes, bytes, slice.dirty.size()};
        }

        [[nodiscard]] RenderResult<void>
        prepare(std::uint32_t slot, const PreparationCapacity& capacity, ResourcePreparation& output) noexcept
        {
            if (output.active())
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            auto required = requirements(slot);
            if (!required)
            {
                return cxx::unexpected(required.error());
            }
            if (!capacity.covers(*required))
            {
                return cxx::unexpected(RenderError{kStorageCapacity});
            }
            auto& slice = slices_[slot];
            slice.dirty.swap(slice.staged);
            std::ranges::sort(slice.staged);
            slice.copies.clear();
            for (const auto index : slice.staged)
            {
                const auto offset = std::uint64_t{index} * sizeof(T);
                std::memcpy(slice.frozen.data() + offset, &values_[index], sizeof(T));
                slice.marked[index] = 0;
                const bool is_contiguous = !slice.copies.empty() &&
                                           slice.copies.back().destination_offset + slice.copies.back().bytes == offset;
                if (is_contiguous)
                {
                    slice.copies.back().bytes += sizeof(T);
                }
                else
                {
                    slice.copies.push_back({offset, offset, sizeof(T)});
                }
            }
            slice.preparing = true;
            // All bounds and capacities above are established before changing dirty state.
            if (!output.bind(&slice, &Slice::finish, slice.frozen, slice.copies, capacity.destination_bytes))
            {
                std::terminate();
            }
            return {};
        }

        [[nodiscard]] static const ResourcePreparationOps& preparationOps() noexcept
        {
            static const ResourcePreparationOps ops{
                [](const void* data, std::uint32_t slot) noexcept
                { return static_cast<const ReplicatedData*>(data)->requirements(slot); },
                [](void* data, std::uint32_t slot, const PreparationCapacity& capacity, ResourcePreparation& result
                ) noexcept { return static_cast<ReplicatedData*>(data)->prepare(slot, capacity, result); }
            };
            return ops;
        }

    private:
        std::vector<T> values_;
        std::vector<Slice> slices_;
        bool quarantined_ = false;
    };
} // namespace lux::render::detail
