#pragma once

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/common/Size2D.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>

#include <entt/entity/entity.hpp>

#include <cstddef>
#include <cstdint>

namespace lux::runtime
{
    namespace detail
    {
        class PrimaryViewPresentationSystem;
    }
    class RenderSceneIntegration;

    enum class EPrimaryViewPresentationStatus : std::uint8_t
    {
        DISABLED,
        NO_PRIMARY_CAMERA,
        AMBIGUOUS_PRIMARY_CAMERA,
        TARGET_UNAVAILABLE,
        BOUND
    };

    struct PrimaryViewPresentationSnapshot final
    {
        EPrimaryViewPresentationStatus status{
            EPrimaryViewPresentationStatus::DISABLED};
        std::size_t candidate_count{0u};
        entt::entity bound_camera{entt::null};
        lux::render::RenderTargetId target{};
        lux::common::Size2D extent{};
        std::uint64_t intent_revision{1u};
        std::uint64_t committed_revision{0u};
        bool command_pending{false};
    };

    /// Main-owner service containing only the host's output intent and an
    /// inspectable result. Cooked scene content never sees RenderTargetId.
    class LUX_RUNTIME_RENDER_SCENE_PUBLIC PrimaryViewPresentation final
    {
    public:
        PrimaryViewPresentation(
            bool enabled,
            lux::render::RenderTargetId target,
            lux::common::Size2D extent) noexcept
            : intent_{enabled, target, extent, 1u}
        {
            snapshot_.status = enabled
                ? EPrimaryViewPresentationStatus::NO_PRIMARY_CAMERA
                : EPrimaryViewPresentationStatus::DISABLED;
            snapshot_.target = target;
            snapshot_.extent = extent;
        }

        [[nodiscard]] const PrimaryViewPresentationSnapshot& snapshot()
            const noexcept
        {
            return snapshot_;
        }

        /// Owner-main-thread host intent. The system observes the revision and
        /// performs all registry mutation at the Schedule command barrier.
        void setOutputIntent(
            lux::render::RenderTargetId target,
            lux::common::Size2D extent) noexcept
        {
            updateIntent(target, extent);
        }

    private:
        friend class detail::PrimaryViewPresentationSystem;
        friend class RenderSceneIntegration;

        struct Intent final
        {
            bool enabled{false};
            lux::render::RenderTargetId target{};
            lux::common::Size2D extent{};
            std::uint64_t revision{1u};
        };

        void updateIntent(
            lux::render::RenderTargetId target,
            lux::common::Size2D extent) noexcept
        {
            auto next_extent = intent_.extent;
            if (extent.width != 0u && extent.height != 0u)
                next_extent = extent;
            if (intent_.target == target &&
                intent_.extent.width == next_extent.width &&
                intent_.extent.height == next_extent.height)
                return;
            intent_.target = target;
            intent_.extent = next_extent;
            ++intent_.revision;
            if (intent_.revision == 0u)
                intent_.revision = 1u;
            snapshot_.target = intent_.target;
            snapshot_.extent = intent_.extent;
            snapshot_.intent_revision = intent_.revision;
        }

        Intent intent_{};
        PrimaryViewPresentationSnapshot snapshot_{};
    };
}
