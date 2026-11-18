#pragma once

#include <lux/engine/render/extensions/resource/ResourceBinding.hpp>
#include <lux/engine/render/visibility.h>
#include <utility>
#include <vector>

namespace lux::render::detail
{
    // Embedded in the real frame operation. Owns only this round's frozen Resource borrows,
    // not native buffers, a submission queue, or the large-asset upload task machinery.
    class LUX_RENDER_PUBLIC Preparation final
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

        using Submitted = void (*)(void*, std::uint32_t, RenderResult<void>) noexcept;
        // Borrowed replica/Feature settlement. The actual frame owns this record, not the Uploader.
        void onSubmitted(void*, Submitted) noexcept;
        // The owner supplies the actual native result, after staging has committed or cancelled.
        // New callbacks appended during delivery wait for the next settlement of this preparation.
        void finishCallbacks(std::uint32_t slot, RenderResult<void>) noexcept;

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
        bool delivering_ = false;
        std::vector<std::pair<void*, Submitted>> receivers_;
        std::vector<const ResourceBinding*> order_;
        std::vector<ResourceBinding> selected_;
        std::vector<ResourcePreparation> frozen_;
        std::size_t capacity_;
        EState state_ = EState::IDLE;
    };
} // namespace lux::render::detail
