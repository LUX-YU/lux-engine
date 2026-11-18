#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <span>
#include <utility>

namespace lux::render::detail
{
    struct CopyRange final
    {
        std::uint64_t source_offset = 0;
        std::uint64_t destination_offset = 0;
        std::uint64_t bytes = 0;

        [[nodiscard]] bool fits(std::uint64_t upload_bytes, std::uint64_t destination_bytes) const noexcept
        {
            const bool has_source = source_offset <= upload_bytes;
            const bool has_destination = destination_offset <= destination_bytes;
            return bytes != 0 && has_source && has_destination && bytes <= upload_bytes - source_offset &&
                   bytes <= destination_bytes - destination_offset;
        }

        bool operator==(const CopyRange&) const noexcept = default;
    };

    // Available staging space/ranges for the complete round, already reserved by its native owner.
    struct PreparationBudget final
    {
        std::uint64_t upload_bytes = 0;
        std::size_t copy_ranges = 0;
    };

    struct PreparationCapacity final
    {
        std::uint64_t upload_bytes = 0;
        std::uint64_t destination_bytes = 0;
        std::size_t copy_ranges = 0;

        [[nodiscard]] bool covers(const PreparationCapacity& required) const noexcept
        {
            return upload_bytes >= required.upload_bytes && destination_bytes >= required.destination_bytes &&
                   copy_ranges >= required.copy_ranges;
        }

        bool operator==(const PreparationCapacity&) const noexcept = default;
    };

    enum class EPreparationOutcome : std::uint8_t
    {
        ACCEPTED,
        REJECTED,
        UNKNOWN
    };

    // One actual frozen Resource slice borrow. Source bytes/ranges belong to the professional
    // Storage; native staging/destination buffers remain owned by Uploader/Storage respectively.
    class ResourcePreparation final
    {
    public:
        using Complete = void (*)(void*, EPreparationOutcome) noexcept;
        ResourcePreparation() noexcept = default;

        ~ResourcePreparation() noexcept
        {
            cancel();
        }

        ResourcePreparation(const ResourcePreparation&) = delete;
        ResourcePreparation& operator=(const ResourcePreparation&) = delete;

        ResourcePreparation(ResourcePreparation&& other) noexcept
        {
            move(other);
        }

        ResourcePreparation& operator=(ResourcePreparation&& other) noexcept
        {
            if (this != &other)
            {
                cancel();
                move(other);
            }
            return *this;
        }

        // Called by the professional freezer after complete capacity/identity qualification.
        [[nodiscard]] RenderResult<void>
        bind(
            void* owner,
            Complete complete,
            std::span<const std::byte> source,
            std::span<const CopyRange> copies,
            std::uint64_t destination_bytes
        ) noexcept
        {
            const bool is_invalid_owner = active() || !owner || !complete;
            if (is_invalid_owner)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            for (const auto& copy : copies)
            {
                const bool is_invalid_range = !copy.fits(source.size(), destination_bytes);
                if (is_invalid_range)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
            }
            owner_ = owner;
            complete_ = complete;
            source_ = source;
            copies_ = copies;
            return {};
        }

        [[nodiscard]] bool active() const noexcept
        {
            return owner_ != nullptr;
        }

        [[nodiscard]] std::span<const std::byte> source() const noexcept
        {
            return source_;
        }

        [[nodiscard]] std::span<const CopyRange> copies() const noexcept
        {
            return copies_;
        }

        [[nodiscard]] bool fits(const PreparationCapacity& capacity) const noexcept
        {
            if (copies_.size() > capacity.copy_ranges)
            {
                return false;
            }
            for (const auto& copy : copies_)
            {
                if (!copy.fits(capacity.upload_bytes, capacity.destination_bytes))
                {
                    return false;
                }
            }
            return true;
        }

        void beginSubmit() noexcept
        {
            const bool is_invalid_phase = !active() || submitting_;
            if (is_invalid_phase)
            {
                std::terminate();
            }
            submitting_ = true;
        }

        void settle(EPreparationOutcome outcome) noexcept
        {
            const bool is_valid_outcome = outcome == EPreparationOutcome::ACCEPTED ||
                                          outcome == EPreparationOutcome::REJECTED ||
                                          outcome == EPreparationOutcome::UNKNOWN;
            const bool is_invalid_phase =
                !active() || !is_valid_outcome || (!submitting_ && outcome != EPreparationOutcome::REJECTED);
            if (is_invalid_phase)
            {
                std::terminate();
            }
            auto* owner = std::exchange(owner_, nullptr);
            auto complete = std::exchange(complete_, nullptr);
            source_ = {};
            copies_ = {};
            submitting_ = false;
            complete(owner, outcome);
        }

        // Only pre-native cancellation. An in-flight operation must adopt its real return.
        void cancel() noexcept
        {
            if (submitting_)
            {
                std::terminate();
            }
            if (active())
            {
                settle(EPreparationOutcome::REJECTED);
            }
        }

    private:
        void move(ResourcePreparation& other) noexcept
        {
            if (other.submitting_)
            {
                std::terminate();
            }
            owner_ = std::exchange(other.owner_, nullptr);
            complete_ = std::exchange(other.complete_, nullptr);
            source_ = std::exchange(other.source_, {});
            copies_ = std::exchange(other.copies_, {});
        }

        void* owner_ = nullptr;
        Complete complete_ = nullptr;
        std::span<const std::byte> source_;
        std::span<const CopyRange> copies_;
        bool submitting_ = false;
    };

    struct ResourcePreparationOps final
    {
        using Requirements = RenderResult<PreparationCapacity> (*)(const void*, std::uint32_t) noexcept;
        using FreezeResult = RenderResult<void>;
        using Freeze =
            FreezeResult (*)(void*, std::uint32_t, const PreparationCapacity&, ResourcePreparation&) noexcept;
        Requirements requirements = nullptr;
        Freeze freeze = nullptr;
    };

    // Per-round, cold-qualified physical capacity and Resource identity. No factory/type-name lookup.
    struct ResourceBinding final
    {
        ResourceInstanceId identity;
        void* resource = nullptr;
        const ResourcePreparationOps* ops = nullptr;
        PreparationCapacity capacity;

        [[nodiscard]] bool valid() const noexcept
        {
            return !identity.isNull() && resource && ops && ops->requirements && ops->freeze;
        }

    };
} // namespace lux::render::detail
