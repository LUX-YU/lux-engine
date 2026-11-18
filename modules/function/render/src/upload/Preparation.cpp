#include <algorithm>
#include <lux/engine/render/extensions/upload/Preparation.hpp>

namespace lux::render::detail
{
    Preparation::Preparation(std::size_t resource_capacity) noexcept : capacity_(resource_capacity)
    {
        receivers_.reserve(resource_capacity);
        order_.reserve(resource_capacity);
        selected_.reserve(resource_capacity);
        frozen_.reserve(resource_capacity);
    }

    Preparation::~Preparation() noexcept
    {
        const bool has_pending_delivery = delivering_ || !receivers_.empty();
        if (has_pending_delivery) std::terminate();
        cancel();
    }

    void Preparation::onSubmitted(void* receiver, Submitted complete) noexcept
    {
        const bool is_invalid_receiver = complete == nullptr;
        const bool is_invalid_state = quarantined();
        const bool is_invalid_registration = is_invalid_receiver || is_invalid_state;
        if (is_invalid_registration) std::terminate();
        receivers_.emplace_back(receiver, complete);
    }

    void Preparation::finishCallbacks(std::uint32_t slot, RenderResult<void> result) noexcept
    {
        if (delivering_) std::terminate();
        delivering_ = true;
        const auto boundary = receivers_.size();
        for (std::size_t i = 0; i < boundary; ++i)
        {
            const auto [receiver, complete] = std::exchange(receivers_[i], {});
            complete(receiver, slot, result);
        }
        receivers_.erase(receivers_.begin(), receivers_.begin() + boundary);
        delivering_ = false;
    }

    RenderResult<void>
    Preparation::freeze(
        std::span<const ResourceBinding> resources,
        std::uint32_t slot,
        PreparationBudget available
    ) noexcept
    {
        if (quarantined())
        {
            return cxx::unexpected(RenderError{kOutcomeUnknown});
        }
        if (state_ != EState::IDLE)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        state_ = EState::PREPARING;
        const auto fail = [&](RenderError error) noexcept -> RenderResult<void>
        {
            frozen_.clear(); // Only known pre-native failures can reach this rollback.
            selected_.clear();
            order_.clear();
            state_ = EState::IDLE;
            return cxx::unexpected(error);
        };
        for (const auto& resource : resources)
        {
            if (!resource.valid())
            {
                return fail(RenderError{kStorageContract});
            }
            order_.push_back(&resource);
        }
        std::ranges::sort(order_, {}, [](const auto* resource) noexcept
        {
            return std::pair{resource->identity.index, resource->identity.gen};
        });
        for (const auto* resource : order_)
        {
            if (!selected_.empty() && selected_.back().identity == resource->identity)
            {
                const auto& owner = selected_.back();
                if (owner.resource != resource->resource || owner.ops != resource->ops)
                {
                    return fail(RenderError{kStorageMismatch});
                }
                if (owner.capacity != resource->capacity)
                {
                    return fail(RenderError{kStorageCapacity});
                }
                continue;
            }
            if (selected_.size() == capacity_)
            {
                return fail(RenderError{kStorageCapacity});
            }
            selected_.push_back(*resource);
        }
        order_.clear();
        // Qualify the entire destination/upload/range budget before freezing any Resource.
        for (auto& resource : selected_)
        {
            auto required = resource.ops->requirements(resource.resource, slot);
            if (!required)
            {
                return fail(required.error());
            }
            if (!resource.capacity.covers(*required))
            {
                return fail(RenderError{kStorageCapacity});
            }
            const bool is_round_full =
                required->upload_bytes > available.upload_bytes || required->copy_ranges > available.copy_ranges;
            if (is_round_full)
            {
                return fail(RenderError{kStorageCapacity});
            }
            available.upload_bytes -= required->upload_bytes;
            available.copy_ranges -= required->copy_ranges;
            resource.capacity = *required; // Freeze may consume only this already reserved quota.
        }
        for (const auto& resource : selected_)
        {
            auto& frozen = frozen_.emplace_back();
            auto result = resource.ops->freeze(resource.resource, slot, resource.capacity, frozen);
            if (!result)
            {
                return fail(result.error());
            }
            const bool is_invalid_snapshot = !frozen.active() || !frozen.fits(resource.capacity);
            if (is_invalid_snapshot)
            {
                return fail(RenderError{kStorageContract});
            }
        }
        state_ = EState::FROZEN;
        return {};
    }

    RenderResult<void> Preparation::beginSubmit() noexcept
    {
        if (state_ != EState::FROZEN)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        for (auto& frozen : frozen_)
        {
            frozen.beginSubmit();
        }
        state_ = EState::SUBMITTING;
        return {};
    }

    RenderResult<void> Preparation::submitted(RenderResult<void> result) noexcept
    {
        if (state_ != EState::SUBMITTING)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        const auto outcome = result                                   ? EPreparationOutcome::ACCEPTED
                             : result.error().type == kOutcomeUnknown ? EPreparationOutcome::UNKNOWN
                                                                      : EPreparationOutcome::REJECTED;
        for (auto& frozen : frozen_)
        {
            frozen.settle(outcome);
        }
        frozen_.clear();
        selected_.clear();
        state_ = outcome == EPreparationOutcome::UNKNOWN ? EState::QUARANTINED : EState::IDLE;
        return result;
    }

    void Preparation::cancel() noexcept
    {
        const bool has_operation = state_ == EState::PREPARING || state_ == EState::SUBMITTING;
        if (has_operation)
        {
            std::terminate();
        }
        if (state_ != EState::FROZEN)
        {
            return;
        }
        frozen_.clear();
        selected_.clear();
        state_ = EState::IDLE;
    }
} // namespace lux::render::detail
