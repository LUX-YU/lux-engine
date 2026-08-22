#pragma once

#include <lux/engine/ecs/animation/systems/AnimationSystem.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>

namespace lux::asset
{
    class AssetManager;
}

namespace lux::runtime
{
    /// Runtime integration that resolves skeletal animation assets and emits
    /// explicit asynchronous demand through the existing AssetClient.
    class SkeletalAnimationResolver final : public lux::ecs::ISystem
    {
    public:
        SkeletalAnimationResolver(
            lux::asset::AssetManager& manager,
            lux::asset_runtime::AssetClient client
        ) noexcept;

        void update(const lux::ecs::SystemUpdateContext& context) override;

        [[nodiscard]] std::span<const lux::ecs::SystemType> runsBefore()
            const noexcept override
        {
            static constexpr lux::ecs::SystemType kBefore[]{
                lux::ecs::systemType<lux::ecs::AnimationSystem>()};
            return kBefore;
        }

    private:
        lux::asset::AssetManager* manager_{};
        lux::asset_runtime::AssetClient client_;
    };
}
