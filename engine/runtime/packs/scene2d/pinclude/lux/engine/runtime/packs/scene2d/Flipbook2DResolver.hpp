#pragma once

#include <lux/engine/ecs/animation/systems/FlipbookAnimationSystem.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>

namespace lux::asset
{
    class AssetManager;
}

namespace lux::runtime
{
    /// Runtime integration that turns 2D animation asset demand into explicit
    /// AssetClient requests while keeping the ECS sampler IO-free.
    class Flipbook2DResolver final : public lux::ecs::ISystem
    {
    public:
        Flipbook2DResolver(
            lux::asset::AssetManager& manager,
            lux::asset_runtime::AssetClient client
        ) noexcept;

        void update(const lux::ecs::SystemUpdateContext& context) override;

        [[nodiscard]] std::span<const lux::ecs::SystemType> runsBefore()
            const noexcept override
        {
            static constexpr lux::ecs::SystemType kBefore[]{
                lux::ecs::systemType<lux::ecs::FlipbookAnimationSystem>()};
            return kBefore;
        }

    private:
        lux::asset::AssetManager* manager_{};
        lux::asset_runtime::AssetClient client_;
    };
}
