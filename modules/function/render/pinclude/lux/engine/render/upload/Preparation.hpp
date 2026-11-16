#pragma once

#include <lux/engine/render/extensions/resource/ResourceBinding.hpp>
#include <vector>

namespace lux::render::detail
{
    // Embedded in the real frame operation. Owns only this round's frozen Resource borrows,
    // not native buffers, a submission queue, or the large-asset upload task machinery.
    class Preparation final
    {
    public:
        explicit Preparation(std::size_t resource_capacity) noexcept;
        ~Preparation() noexcept;
        Preparation(const Preparation&) = delete;
        Preparation& operator=(const Preparation&) = delete;
        [[nodiscard]] RenderResult<void>
        freeze(std::span<const ResourceBinding> resources, std::uint32_t slot, PreparationBudget available) noexcept;
        [[nodiscard]] RenderResult<void> beginSubmit() noexcept;
        // Adopt the actual native outcome. Unknown poisons the entire prepared group, not a retry.
        [[nodiscard]] RenderResult<void> submitted(RenderResult<void> result) noexcept;
        void cancel() noexcept;

        [[nodiscard]] std::span<const ResourceBinding> bindings() const noexcept
        {
            return selected_;
        }

        [[nodiscard]] std::span<const ResourcePreparation> resources() const noexcept
        {
            return frozen_;
        }

        [[nodiscard]] bool quarantined() const noexcept
        {
            return state_ == EState::QUARANTINED;
        }

    private:
        enum class EState : std::uint8_t
        {
            IDLE,
            PREPARING,
            FROZEN,
            SUBMITTING,
            QUARANTINED
        };
        std::vector<ResourceBinding> selected_;
        std::vector<ResourcePreparation> frozen_;
        std::size_t capacity_;
        EState state_ = EState::IDLE;
    };
} // namespace lux::render::detail
